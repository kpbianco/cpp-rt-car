# Handoff

## Active batch: M21-05

Baseline `2821f90fb09dc86820b879bb47a39d8165ff0e0f` is the merged M21-04
sampled-I/O and loopback path. M21-05 adds only the additive C++ mixed-rate
closure policy, fixed logical actions, conditional ordinary checkpoint state,
a separate bounded active-replay artifact, instance-local loopback hooks, and
a reusable installed-surface conformance fixture. Preserve C ABI v8/70
exports, every existing artifact/schema byte contract, support matrices,
CUDA/XDMA candidates, and prior evidence. Do not claim physical I/O,
electrical correctness, controlled timing, HIL, RT1/RT2, Unreal, release, or
deployment.

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. The binding M21-05 contract is
`contracts/active-batch.yaml`, sourced from control revision
`8bf0d8e7a6563fe88246925b44e2bddc77a457fe`.

## M21-05 implementation handoff

`MixedRateClosurePolicy` is configuring-only and freezes bounded action and
replay capacities plus its semantic policy identity. Action capacity is
observational and may be zero; active replay requires positive transcript and
byte limits and an explicitly deterministic backend. The 256-byte action
records and runtime-bound cursors are distinct from rate-action schema 1 and
global observability schema 2. Any action gap, overwrite, or drop makes the
captured range replay-ineligible.

Closure-enabled checkpoints append `rtfw.mixed-rate` as a normal schema-1
state record. Export and active replay require quiescence. The active artifact
is its own fixed little-endian schema and must be fully validated before
restore; legacy `Runtime::replay()` keeps its active-plan rejection. Replay
uses recorded logical decisions, suppresses live watchdog timing, permits only
deterministic mock/loopback backends, re-executes providers, and compares the
complete generated action and content-identity sequence.

The public loopback logical-action log is fixed capacity and instance-local;
reset is allowed only after shutdown. The conformance fixture includes only
installed headers and is shared by repository and relocated-package tests.
Keep caller replay payloads borrowed/explicit, never add sampled payload bytes
to default telemetry, and retain exact MemoryPlan accounting. M21/CAP-M21 may
be called software-complete only after this candidate merges; physical and RT
qualification remain separate.

## M21-03 implementation handoff

Use `CrossRateDeviceEndpointSelector` only for the device side of a
CPU→device or device→CPU channel. Its phase-local payload-reference ordinal and
positive stride are copied and frozen. Finalization derives the backend,
buffer, envelope, role, host access, timestamp domain, and in-flight slot count
from M21-01; it rejects device→device, implicit matching, incoherent/opaque
memory, overlapping envelopes, and partial selectors transactionally.

CPU input is copied before the provider runs. The provider still returns the
exact frozen declaration; Runtime alone materializes the selected slot offset
and payload byte count. Device output is copied and published only after exact
terminal success while the M21-02 slot remains host-owned. Failure, timeout,
cancel, malformed completion, loss, skip, and shed paths do not call the output
publisher. Channel-free M21-02 and ordinary M17 batches retain exact-reference
behavior.

M21-04 owns sampled-I/O descriptors, clocks/triggers, safe outputs, and
overrun/underrun policy. M21-05 consumes those semantics without changing
their HAL or artifact versions. Do not describe this portable fake-driver path
as physical device, DAC/DAQ, HIL, RT1, or RT2 evidence.

## M21-02 implementation handoff

M21-01's admitted model remains the only source of backend, command, buffer,
timeline, completion-budget, and capacity identity. M21-02 adds immutable
reference-to-device and dependency slices plus preallocated completion tickets.
Do not derive execution identity from provider output or rescan configuring
registries after start.

An active device provider receives `DeviceCallbackContext::rate_release`, runs
once on the selected executor path, and enqueues a complete copied batch into
the existing per-backend submission lane. It does not call a vendor entry or
wait for completion. Independent records in one release group may overlap;
precomputed device dependencies and the group terminal barrier prevent a
dependent CPU/device record or successful step from advancing early.

Submitted timeouts are settled once by the existing service lane and retain
their slot in quarantine through checked backend shutdown. The exact batch and
release identity prevents a late completion from settling a reused slot.
Ordinary M17 batches retain executor-token completion behavior. M21-02 added no
new telemetry schema, replay encoding, lane, or hardware/RT qualification and
remains the ownership foundation consumed by M21-03.

## M20-PRE-01 implementation handoff

Use `scripts/verify-portable-assurance.sh` with an explicit build directory and
complete source commit. The five modes are `dependencies`, `static`, `fuzz`,
`artifacts`, and cumulative `all`. Source manifests, seed manifests,
dictionaries, dependency/action pins, the Clang 14 policy, SPDX schema, public
fixture trust snapshot, and artifact policy are checked-in inputs. Generated
reports and candidates must stay below the explicit build/CI evidence root.

The package candidate is staged twice to prove byte-stable SBOM generation,
then bound to an unsigned in-toto statement and a strict expected-source final
manifest before extraction and relocated consumption. The signed fixture is
fictional non-target material used only to prove offline cryptographic positive
and mutation rejection. No RTFW candidate is signed or authenticated.

Do not broaden this batch into production source, ABI or schema changes,
support/qualification promotion, a release workflow, target signing,
continuous-service fuzzing, controlled performance, soak, hardware, RT, or
Unreal work. Missing Clang 14/CMake/libFuzzer tooling is an unperformed gate,
not a pass. Hosted CI and separate human supply-chain, fuzz/analyzer,
compatibility, and claim review remain mandatory before merge.

## M19-01 implementation handoff

`rt/extension_abi.h` is the independently versioned C11 ABI v1. Runtime takes
only an already-resolved entry pointer and transactionally stages fixed CPU
phases, device-v1 backends, services, resources, and local relationships.
Generational owner/kind/slot handles reject failed, foreign, wrong-kind, stale,
and detached uses. The full ABI layout, copied/borrowed matrix, lifecycle,
control-thread rule, retry order, accounting, identity, evolution, and trust
boundary are in `docs/extension_registration.md`.

Do not add a loader or release extension code until `stop()` and
`detach_extension()` both succeed. M19-03 owns Unreal and host module unload
orchestration. M19 and CAP-M19 remain incomplete. M18 remains unpromoted.

## Implemented boundary

M17-04 keeps the CUDA and XDMA `api()` device-ABI-v1 candidate paths exact and
adds `hal_v2_registration(std::string_view) noexcept` accessors backed by the
existing HAL core v2, memory/topology v1, and command/timeline v1 contracts.
Each candidate object is borrowed through one selected path until checked
shutdown succeeds. Driver tables preserve their version-1 aggregate prefixes
and defaults; exact version 2 adds CUDA Graph launch or XDMA control/event/stop
callbacks only when the complete required configuration is present.

CUDA native-v2 uses an explicit staged domain, one configured command stream,
and fixed completion events. It accepts at most 16 caller-owned pre-instantiated
graph handles, unique 16-bit nonzero IDs, and eight copied bindings per graph.
`0x43470000 | graph_id` is the stable dispatch identity. Batch copies, kernels,
and graphs execute in order; partial enqueue, timeout, event failure, loss,
reset, and stop retain every possibly referenced owner until safe drain.

XDMA native-v2 retains H2C/C2H and adds bounded 32-bit little-endian control
read/write plus one finite user-event wait. Stable opcodes encode the checked
word offset or event index. A configured aperture is at most 262144 bytes and
there are at most 16 events. The Linux adapter copies the optional paths,
opens them close-on-exec, wakes event waits through an idempotent stop request,
and never closes a descriptor while a fixed I/O worker may reference it.

Backend-private storage remains fixed and reported through existing capability
bytes. Runtime-owned command storage, submission/service lanes, six MemoryPlan
rows, three provider regions, schemas, installed inventory, release, support,
and license remain unchanged.

## Protected decisions

- Preserve C ABI v8, 70 exports/fingerprint, SONAME 8, device ABI v1, HAL core
  v2, memory extension v1, command extension v1, Runtime statuses, and schemas.
- Preserve exact device-ABI-v1 CUDA/XDMA behavior and conditional identity when
  native registration is unused.
- Keep vendor work off executor workers and within the existing Runtime
  submission/service lanes or fixed XDMA I/O team.
- Never infer graph ownership, CUDA pinning/device memory/coherency, a safe FPGA
  register map, interrupt latency, driver cancellation, or direct peer DMA.
- Keep cross-backend timelines, direct peer paths, device-rate payloads and
  sampled I/O, physical combined qualification, support promotion, release,
  and deployment deferred.

## M17-06 implementation handoff

The sole production repair is in `rt/src/command_batch.cpp`: capability
discovery retains the value-initialized size/version prefix instead of
overwriting it with zero. Complete output validation and existing statuses are
unchanged. `tests/test_command_batch.cpp` observes the exact incoming header
and zero tail and proves every malformed/error/exception attempt publishes
nothing before a corrected retry. `tests/test_vendor_hal_v2.cpp` registers the
actual CUDA and XDMA native candidates together through canonical Runtime and
reaches checked stop.

`samples/cpu_gpu_fpga_cpu.cpp` and its standalone focused test use only the
existing public Runtime/CUDA/XDMA targets. The five-phase graph is CPU prepare,
simulated CUDA upload/Graph/download, disjoint host-stage bridge, simulated
XDMA H2C/control/event/C2H, and CPU validate. Storage is fixed, timelines are
backend-local, complete steps are measured allocation-free, and exact
operation/thread/causality counts cover two frames plus CUDA failure, XDMA
event timeout/cancellation, malformed declarations, correction, isolation,
and unresolved-only cleanup retry.

Retain the M17-05 failed review unchanged; M17-06 is the approved repair, not a
rewrite of that evidence. The M17-06 retained evidence must distinguish local
results from mandatory hosted CI and human review. No physical Graph, device
memory, pinning, coherency, DMA, MMIO, event interrupt, timing, HIL, field,
RT1/RT2, Unreal, signing, release, deployment, or production claim follows.
Exact acceptance and validation results are in
`docs/evidence/M17-06-2026-08-23.md`.

## M18-01 implementation handoff

`qualification/schemas/` contains independent version-1 plan, record, review,
and proposal schemas. `tools/qualification.py` uses only Python 3.11 standard
library facilities and performs strict bounded parsing, exact-byte plan/record/
review binding, canonical embedded-manifest hashing, complete trial and
threshold comparison, artifact-tree verification, and atomic non-overwriting
proposal output. `tests/qualification_fixtures/` contains only synthetic data.

The precise bounds, canonicalization, evolution, review, commands, rollback,
and claim boundary are in `docs/qualification.md`. Existing M12 CUDA/XDMA
`evidence_only` records remain raw inputs only. The tool does not mutate a
support matrix and does not authenticate reviewer attribution or prove plan
chronology. The unchanged M18-01 tool still rejects non-synthetic combined
promotion; a later approved qualification-policy batch owns any revision.

After local verification, mandatory GitHub CI and human schema, security,
qualification, compatibility, and claim-boundary review remain external merge
gates. No physical campaign or support promotion belongs to M18-01.

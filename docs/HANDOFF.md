# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M20-PRE-01 starts from exact target
baseline `c846cc427c93018175a69c7372c130f00c0b713b`, the merged M17-06 result.
The binding M20-PRE-01 contract is `contracts/active-batch.yaml`, sourced from
control revision `acdbdc50f66fd66f9a8e48eee75923edfd759787`.

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
- Keep cross-backend timelines, direct peer paths, device-rate execution,
  physical combined qualification, support promotion, release, and deployment
  deferred.

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

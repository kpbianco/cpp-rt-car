# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M19-01 starts from exact target
baseline `5eb8f101f692b2aff85f82f4a1660630db750706`. M17 remains active and
incomplete because M17-05 is blocked. The binding M19-01 contract is
`contracts/active-batch.yaml`, sourced from control revision
`e2ce44f28d3d3f92d7a8bf4d4f1c4ba7b563a185`.

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
orchestration. M19 and CAP-M19 remain incomplete. M17-05 remains blocked and
M18 remains unpromoted.

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
- Keep the combined sample, cross-backend timelines, device-rate execution,
  physical qualification, support promotion, release, and deployment deferred.

## M17-05 blocker

Canonical Runtime command/timeline discovery clears the capability output
header before calling the backend, but the native CUDA and XDMA candidate
callbacks reject an incoming zero `struct_size`. The required M17-05 use of
the actual candidates through `hal_v2_registration()` therefore fails before
graph configuration. The active batch forbids the needed `rt/src/` repair and
forbids a substitute HAL implementation. Do not implement around this
boundary; first approve a repair batch or explicitly revise M17-05 scope.

The failed prototype, exact commands, rollback, acceptance status, and claim
boundary are retained in `docs/evidence/M17-05-2026-08-12.md`. The combined
sample, focused behavioral test, mandatory CI, and human review remain
unperformed. Do not commit, push, open a pull request, release, or deploy.

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
chronology. Non-synthetic combined promotion remains blocked on M17-05.

After local verification, mandatory GitHub CI and human schema, security,
qualification, compatibility, and claim-boundary review remain external merge
gates. No physical campaign or support promotion belongs to M18-01.

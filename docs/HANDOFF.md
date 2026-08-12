# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M17-04 starts from exact target
baseline `122f9919674b5f09e321deb8ba6701f78b2993ce`; M17-03 is merged in PR
219 at `0d1c95a4e859c9136b28b912735610cd58016e95`, and the baseline changes only
portfolio-harness metadata after that merge. M17 remains active and incomplete.
The binding contract is `contracts/active-batch.yaml`, sourced from control
revision `8f94ced14abf34d7818c44edc73ae3cd204d5b6b`.

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

## Completion output

Run the exact local validation contract through `./scripts/agent-verify.sh
full`, package consumers, C ABI verification, and the SONAME check. Record
acceptance mapping, exact commands/results, identity/accounting facts,
lifecycle and rollback behavior, residual risks, and unperformed validation in
`docs/evidence/M17-04-2026-08-12.md`. Mandatory CI and human review remain
external. Do not commit, push, open a pull request, release, or deploy.

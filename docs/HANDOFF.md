# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M17-03 starts from exact target
baseline `ee1ddd21c483687c4237f3b53ec8d15029d20ff2`; M17-01 and M17-02 are
merged prerequisites. M17 remains active and incomplete. The binding contract
is `contracts/active-batch.yaml`, sourced from control revision
`048c26e8656a4cfce7afd176bafa1476ab11195b`.

## Implemented boundary

M17-03 appends optional command/timeline extension version 1 to the existing
C++ HAL-v2 backend registration without changing its `{name, api,
memory_topology}` aggregate prefix. Runtime copies its fixed capability table
and requires submit, bounded nonblocking poll, cancel, and nonblocking stop
request callbacks. Malformed sizes, versions, capacities, callbacks, reserved
data, exceptions, and partial outputs fail transactionally.

The public bounds are 16 commands, eight waits, eight signals, 16 timelines,
128 inline payload bytes, and eight references per command. Configuring-only
timeline and batch-phase registration is instance/backend bound. Providers run
on the ordinary CPU graph path and fill a caller-borrowed batch, but no backend
function executes there. Runtime validates declared shape, dynamic bounds,
prior-accepted waits, increasing signals, access/ranges, and ordered explicit
flush/invalidate or staged copy operations.

Each opted-in backend has one fixed Runtime-owned submission thread using role
value 6. Bounded admission copies into preallocated slots; the submission lane
alone calls submit and the existing service lane alone polls completions.
Whole completion batches require known unique IDs, exact declared signal sets,
valid statuses, and the declared M17-02 timestamp domain. Timeout, cancel,
submit failure/exception, malformed completion, stop, and reset/loss paths are
terminal and exact-once. A blocked submit is released through stop request and
cancel; no lane detaches.

All new state is included once in device control and the existing logical
extent ledger. Submission stacks are included once in runtime-stack
accounting. The six-row memory plan, three provider regions, schemas, installed
inventory, release, support, and license remain unchanged. Batch capabilities,
timeline descriptors, command order, operations, references, and point handles
are conditional compatibility identity; mutable progress and runtime values
are excluded.

## Protected decisions

- Preserve C ABI v8, 70 exports/fingerprint, SONAME 8, device ABI v1, HAL core
  v2, memory extension v1, Runtime status values, and all existing schemas.
- Preserve the legacy single-submit path and no invented capability for
  adapted-v1, core-only-v2, and memory-only-v2 backends.
- Keep command queues fixed, same-backend only, explicitly synchronized, and
  free of executor-worker vendor calls, hidden work, spill, or detached lanes.
- Keep CUDA/XDMA-v2 controls, CUDA Graph, XDMA MMIO/events, device-rate work,
  cross-backend timelines, combined execution, and direct peer DMA deferred.
- Do not claim hardware, HIL, field, bounded vendor latency, RT1/RT2, Unreal,
  signing, release, staging, deployment, or production validation.

## Completion output

Run the exact local validation contract through `./scripts/agent-verify.sh
full`, C ABI verification, and the SONAME check. Record acceptance mapping,
commands/results, identity/accounting facts, lifecycle and rollback behavior,
residual risks, and unperformed validation in
`docs/evidence/M17-03-2026-08-11.md`. Mandatory CI and human review remain
external. Do not commit, push, open a pull request, release, or deploy.

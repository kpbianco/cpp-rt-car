# Current state

Last audited: 2026-08-11
Batch baseline: `ee1ddd21c483687c4237f3b53ec8d15029d20ff2`

## Product state

- Release 1.2.1 remains the supported portable RT0 product.
- Stable C ABI v8 remains exactly 70 exports with SONAME 8 and its frozen
  fingerprint. Device ABI v1, HAL core v2, and memory/topology extension v1
  are unchanged.
- Runtime-profile schema 7 and its 25 keys, global observability schema 2,
  checkpoint/input-log schema 1, and rate-action schema 1 are unchanged.
- The installed header and target inventory, 1.x aliases, support matrices,
  and Apache-2.0 license are unchanged.
- M14, M14.1, M15, and M16 are complete. M17-01 and M17-02 are merged in
  target history. M17 is active and incomplete; M17-03 is the approved batch.

## M17-03 implementation

The existing installed `rt/device.hpp` now contains optional command/timeline
extension version 1 beside unchanged HAL core v2 and memory extension v1. Its
fixed public limits are 16 commands, eight waits, eight signals, 16 timelines
per opted-in backend, 128 payload bytes, and eight buffer references per
command. Dispatch, copy-to/from-device, flush, and invalidate are explicit;
Runtime never inserts synchronization or infers direct peer DMA.

`HalV2BackendRegistration` preserves its M17-02 positional prefix and appends
one borrowed extension pointer. Runtime validates and copies the table and
capabilities transactionally. Core-only v2, memory-only v2, adapted device ABI
v1, mock, CUDA candidate, and XDMA candidate paths expose no invented batch
capability and keep the legacy single-submit path.

Configuring-only APIs register instance- and backend-bound named timelines and
batch-device phases. A copied declaration fixes command order, operations,
logical buffer references, and wait/signal handles for compatibility identity;
each provider supplies bounded payload, timeout, and timeline values. Runtime
requires a finite nonzero timeout, one to 16 commands, one to eight signals,
same-backend points, prior-accepted waits, strictly increasing signals, valid
ranges/access, and explicit ordered synchronization before queue ownership.

Every opted-in backend receives exactly one precreated Runtime submission lane
with stable role `thread.device-submission`/numeric value 6. The provider runs
as an ordinary CPU graph phase but calls no backend function. A bounded
one-attempt admission copies accepted work into fixed slots; full or contended
storage returns `device_queue_full`. Only the submission lane calls the
possibly blocking extension submit operation, while the existing device
service lane performs bounded completion polling and whole-poll validation.

Completion validates batch identity, exact signal set, status, and the M17-02
completion timestamp domain before atomically publishing all timeline values.
Early completion, timeout, cancellation, malformed output, submit exception,
loss/reset status, blocked stop request, and reverse cleanup all produce one
terminal graph result without detached work. Stop closes admission, performs
the nonblocking backend stop request, resolves work, joins submission lanes in
reverse order, then stops the service lane and existing memory/backend owners.

Tables, capabilities, timeline state, queue slots, copied batches, completion
scratch, lane controls, and validation state are fixed before start and counted
once in `device_control_bytes`. Submission stacks remain in the existing M15
runtime-stack row. The six-row `MemoryPlan` equation and three provider-backed
regions do not change. Extension/timeline/declaration semantics enter graph
identity conditionally; mutable IDs, progress, timestamps, callbacks, and
addresses do not.

## Boundary and next action

M17-03 establishes portable RT0 contract behavior with static, unit,
failure-injected, sanitizer, package, and synthetic memory-order evidence only.
M17 and CAP-M17 remain incomplete. Native CUDA/XDMA-v2 controls, CUDA Graph,
XDMA MMIO/events, device-rate execution, cross-backend timelines, combined
execution, direct peer DMA, and physical or RT qualification remain deferred.

Run every command in `contracts/active-batch.yaml`, retain exact results in
`docs/evidence/M17-03-2026-08-11.md`, and leave mandatory GitHub CI and human
API, compatibility, concurrency, lifetime, accounting, security, and claim
review as external gates. Do not infer hardware, HIL, field, latency, RT1/RT2,
Unreal, signing, release, deployment, or production evidence.

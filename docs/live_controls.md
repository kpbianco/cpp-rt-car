# Live-control staging contract

M22-01 provides an additive C++ staging substrate inside `rt::Runtime`. It is
data-only: no staged update is scheduled, interpreted, applied, checkpointed,
replayed, rolled back, or emitted as telemetry in this batch. Exact-boundary
consumption and ordering belong to M22-02.

## Configuration and ownership

`LiveControlPolicy` is copied once while Runtime is configuring. Its schema and
structure size are exact, reserved bytes are zero, the admission rule is
`reject_new`, and reset behavior is `discard_with_runtime`. A policy whose
identity and every capacity are zero disables the complete surface. An enabled
policy has a positive identity and positive bounded capacities no greater than:

| Resource | Absolute limit |
| --- | ---: |
| Mailboxes | 64 |
| Producers | 256 |
| Total records | 65,536 |
| Payload bytes per record | 65,536 |
| Total payload storage | 1 GiB |

Mailbox declarations assign a positive identity, record capacity, and payload
stride. Producer declarations assign a globally unique positive producer
identity, exactly one mailbox, and a positive initial sequence below
`UINT64_MAX`. Declarations are copied, duplicates and checked-arithmetic
overflow are rejected, and finalization fails transactionally if the complete
set is absent, malformed, over capacity, or over the Runtime memory budget.

Successful finalization allocates every mailbox control, record slot, payload
slot, producer sequence, counter, and compiled rate-target copy. It then issues
producer handles bound to a non-reused 64-bit Runtime identity, configuration
generation, mailbox, producer, and producer index. A handle from another
Runtime or generation cannot address local storage. A stopped Runtime is
terminal; a new Runtime receives a different identity and fresh storage.

## Canonical record

`LiveControlUpdateRecord` is exactly 128 bytes and contains only fixed-width
values. An input record has mailbox sequence zero. Runtime assigns a positive
mailbox sequence only after a successful reservation and complete payload copy.
The record carries:

- exact schema and record size;
- Runtime/configuration generation and mailbox/producer identities;
- positive expected producer sequence;
- host-frame or compiled-rate-release target identity;
- one closed update kind;
- exact payload byte count, alignment, FNV-1a 64-bit digest, and canonical
  little-endian policy flag; and
- zero reserved bytes.

A host-frame target supplies a finite frame index and leaves all rate fields at
their invalid sentinels. A rate target leaves the frame field at its sentinel
and exactly repeats one finalization-time reference release: reference index,
rate-domain registration index, phase index, domain release sequence, and
substep ordinal. M22-01 validates structural membership only. Whether a target
is late, missed, replaceable, or ordered before another update is deliberately
undefined until M22-02.

The closed update kinds are scenario parameters, controller parameters, sensor
calibration, fault configuration, and clear fault. Only clear fault permits an
empty payload. The digest covers exactly the supplied bytes; callers compute
it with `live_control_payload_digest()`. Payload contents remain opaque. The
runtime performs no application parsing, authorization callback, signature
verification, file/network access, executable loading, or vendor call.

## Admission and publication

`stage_live_control_update()` is an explicit non-RT producer API. Runtime
callbacks cannot call it. It validates the handle, producer sequence, complete
record structure, target, and payload before attempting the mailbox claim.
The mailbox uses one atomic claim attempt; contention returns `busy` without a
mutex, wait, retry loop, allocation, alternate queue, or overwrite.

After a successful claim, Runtime rechecks admission and producer sequence,
then checks fixed capacity. It copies the payload into the selected
Runtime-owned stride, zeroes unused bytes, writes the immutable record, and
publishes the slot with release ordering. Only then does it advance occupancy,
mailbox sequence, producer sequence, and the accepted counter. Inspectors use
acquire ordering and never expose an unpublished slot. A failed copy cannot
publish because fixed validated byte copies do not invoke user code or allocate.

Outcomes are distinct:

| Outcome | Meaning |
| --- | --- |
| `accepted` | One complete immutable slot was published. |
| `invalid` | Record, target, flags, reserved bytes, or payload structure failed. |
| `full` | Capacity was already committed; no existing slot changed. |
| `busy` | The single bounded claim attempt observed another producer. |
| `stale` | Handle ownership/generation or expected producer sequence disagreed. |
| `stopped` | Stop had closed new admission. |
| `exhausted` | A producer or mailbox sequence reached its no-wrap sentinel. |

Foreign Runtime handles fail before touching a local mailbox. Structural
rejection and full/busy/stopped/exhausted outcomes do not consume a record or
advance either sequence. Mailbox counters and occupancy are read-only
inspection state; a full mailbox uses reject-new semantics and never evicts,
coalesces, replaces, or mutates a published record.

## Inspection, identity, and accounting

Runtime exposes mailbox counts, one fixed mailbox-info snapshot, immutable
record lookup by mailbox sequence, and exact-size copying into caller-owned
payload storage. Inspection returns no raw internal address, does not advance
execution or a consumer cursor, and leaves caller output unchanged when the
requested sequence is unavailable or the payload destination has the wrong
extent.

Frozen schema/legal tables, policy, capacities, admission/reset rules, and
sorted mailbox/producer declarations participate in graph, configuration, and
replay compatibility identity. Runtime identity, caller addresses, staged
payload bytes, arrival order, occupancy, counters, and inspection calls do not.

`MemoryPlan` reports mailbox/producer counts, total record capacity, exact
payload bytes, and the complete live-control heap contribution. Policy and
declaration storage, mailbox controls, atomics/counters, immutable record
slots, payload slots, producer state, and copied compiled-rate targets are
included once in `runtime_control_bytes`, `planned_bytes`, and the exact logical
control-extent ledger. No provider-backed memory region or execution lane is
added.

## Lifecycle and deferred behavior

Admission may occur after finalization, before or during running execution,
from bounded external producer threads. It is not signal-handler or interrupt
safe. `step()`, `run_periodic()`, active mixed-rate dispatch, sampled I/O,
device completion, checkpoint, replay, telemetry, watchdog, and state registry
paths never inspect or consume staged slots. Staging therefore cannot change
callback behavior, artifacts, mixed-rate actions, or existing telemetry.

`stop()` closes admission before checking active execution and existing cleanup
ownership. A producer that already won the claim may finish its bounded copy;
the first stop attempt reports `invalid_state` and must be retried after that
claim quiesces. A new claim observes stopped. Published records remain
inspectable until the terminal Runtime is destroyed and never carry into
another Runtime generation.

This establishes portable RT0 schema, ownership, bounded-admission, copied-
payload, identity, accounting, and inspection behavior only. It is not
evidence for transactional application, rollback, physical control, HIL,
controlled latency, RT1/RT2, executable or Unreal hot reload, support
promotion, release, deployment, or production readiness.

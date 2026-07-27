# Determinism, Checkpoints, and Replay

RTFW 0.9 retains milestone M7 for the target `rt::Runtime` path. It provides
an explicit D1 contract for registered state, a stable little-endian
checkpoint format, a stable input-log format, and bounded checkpoint replay.
It does not qualify arbitrary application code as deterministic, and it does
not establish D2 or D3.

## Supported tiers

| Tier | 0.9 behavior |
| --- | --- |
| D0 — unspecified | Supported. Checkpoints require an exact resolved configuration identity. No schedule-independent state claim is made. |
| D1 — schedule-independent | Supported as an explicit application contract. The same binary, ISA, semantic configuration, seed, input log, and registered-state schema must produce identical registered state across supported worker counts. |
| D2 — reproducible build profile | Reserved and rejected by configuration. The repository has no qualified compiler/dependency/hardware profile. |
| D3 — portable deterministic | Reserved and rejected by configuration. Arbitrary floating-point callbacks are outside this tier. |

Selecting D1 also requires the static deterministic executor and disables the
timing-driven frame watchdog. Throughput scheduling and watchdog degradation
are rejected because their scheduling or elapsed-time observations can change
callback-visible state.

## Registered-state boundary

The runtime snapshots only storage explicitly registered with
`Runtime::register_state()` or `rtfw_register_state()`. Each registration has:

- a unique 1–63 byte stable name from `A-Za-z0-9._:/@-`;
- a nonzero application schema version;
- a non-empty, fixed-size, caller-owned byte region.

The storage remains borrowed through runtime destruction. Registration freezes
at finalization, duplicate names and overlapping storage regions are rejected,
and artifact output buffers cannot alias registered state, input records, or
input payloads. Checkpoint and replay calls reject an active frame, periodic
loop, or replay operation.

Registered bytes are an interchange representation, not an invitation to
register native C++ object layouts. Applications must encode integers,
fixed-point values, and explicitly approved floating-point representations in
a canonical form. `rt::store_u32_le`, `rt::store_u64_le`,
`rt::load_u32_le`, and `rt::load_u64_le` are provided for fixed-width
little-endian fields. Compiler padding, pointers, container internals, mutexes,
atomics, and process-local handles must not be registered.

D1 applies only to these registered bytes. Application callbacks remain
responsible for all of the following:

- declaring every shared resource access so unordered conflicting phases are
  rejected;
- avoiding races and execution-order-dependent writes;
- deriving random values from stable seeds and counters;
- avoiding wall clocks, process-global mutable state, device completion order,
  unspecified iteration order, and unapproved math behavior;
- treating queue or scratch exhaustion as a failed qualification run.

## Stable identities

Finalization computes four independent identities:

| Identity | Contents and use |
| --- | --- |
| `config_id` | Complete resolved configuration, including worker count and operational capacities; preserved as provenance |
| `graph_id` | Callback/resource names, dependency edges, and resource-access declarations in registration order |
| `state_schema_id` | Registered state names, schema versions, sizes, and order |
| `replay_id` | Compatibility identity used by restore and replay |

For D0, `replay_id` includes `config_id`, so a worker-count change is
incompatible. For D1, `replay_id` excludes worker count and operational
capacities but includes callback-visible numerical/scratch/overload choices,
the graph, state schema, and workload ID. This is what permits a valid D1
checkpoint to transfer between supported worker counts without discarding its
original `config_id`.

The build ID and complete runtime version are stored as provenance. D1 restore
requires the same runtime major version but does not treat build ID equality as
a D2 claim.

## Checkpoint format v1

Checkpoint artifacts use fixed-width little-endian fields:

- an 8-byte magic and schema-v1 256-byte header;
- runtime version, determinism tier, build/workload IDs, `config_id`,
  `replay_id`, `graph_id`, and `state_schema_id`;
- checkpoint frame index, state count, exact payload/total sizes, registered
  state hash, and whole-artifact checksum;
- one 88-byte record header per state containing the fixed-capacity name,
  schema version, payload size, and payload checksum;
- the canonical state payload immediately after each record header.

FNV-1a-64 checksums detect accidental corruption and stable-artifact drift.
They are not authentication or a defense against a malicious party capable of
recomputing checksums. Untrusted artifacts still require the application's
normal trust boundary.

`inspect_checkpoint_artifact()` validates the complete format without
allocating. Runtime restore additionally validates the frozen runtime
identities, registration order, every name/version/size, and input/output
non-overlap. It performs a full validation pass before copying any byte, so a
malformed, corrupt, truncated, oversized, foreign, or incompatible checkpoint
cannot partially mutate registered state.

`snapshot_max_bytes` is frozen at finalization and cannot exceed the 1 GiB
format safety limit. The runtime never allocates from an encoded length.
Checkpoint output is caller-owned; a short buffer returns
`capacity_exceeded` with the exact required size and writes nothing.

## Input-log format v1

An input log is produced from caller-owned `ReplayInputRecord` entries. Each
entry contains:

- a strictly increasing frame index;
- a nonnegative simulation delta and optional absolute deadline;
- a fixed-width application input type;
- a bounded caller-owned canonical payload.

The 192-byte log header carries runtime version, tier, workload,
`replay_id`, `state_schema_id`, record count, first/last frame, exact sizes,
and a whole-artifact checksum. Each 48-byte record header carries frame fields,
payload size, flags, and a payload checksum.

`replay_input_capacity` and `input_log_max_bytes` bound construction and
parsing. Input-log writers validate every record and compute the complete size
before writing. The C++ codec, inspectors, and replay parsing do not allocate.
The C ABI writer first stages fixed record descriptors in a
`replay_input_capacity`-bounded vector so it can validate C structures before
entering the codec; that non-RT adapter step may return
`resource_exhausted`, but it never allocates from an encoded artifact length.

## Replay sequence and failure semantics

`Runtime::replay()` and `rtfw_replay()` require a running runtime and perform
the following sequence:

1. validate the complete checkpoint;
2. validate the complete input log;
3. verify both artifacts against each other and the finalized runtime;
4. require the first logged frame to follow the checkpoint frame;
5. transactionally restore registered state;
6. invoke the host input callback once per record;
7. synchronously call `step()` with that record's frame context;
8. report processed records, replayed frames, and the final registered-state
   hash.

Malformed or incompatible input is rejected before checkpoint restore. Once a
valid replay begins, an application input callback or frame callback can still
fail; successful earlier replay frames are not rolled back. Applications that
need retry semantics should retain the original checkpoint and invoke replay
again after correcting the callback failure.

All checkpoint, inspection, input-log, hashing, and replay-control calls are
non-RT host operations. Runtime frame callbacks remain allocation-free under
the M4 gate; replay does not turn artifact parsing or host input application
into an RT-lane operation.

## Evidence

The M7 gates include:

- target-runtime state equality across 1, 2, and 4 workers;
- D1 checkpoint transfer between different worker counts;
- checkpoint plus input-log reproduction of the final registered-state hash;
- corrupt/foreign schema and graph rejection with unchanged destination state;
- validation of the input log before restore;
- 5,000 deterministic malformed-parser mutations in the normal test suite;
- a Clang libFuzzer/ASan/UBSan parser target with a bounded CI run;
- allocation instrumentation around checkpoint write, inspect, restore, and
  input-log write;
- C ABI v7 dynamic-loader coverage and C/C++ embedding samples;
- CI artifact exchange that compares checkpoint bytes produced by GCC/Clang
  and FMA-on/FMA-off builds.

The exchanged artifact is a narrow integer-only compatibility fixture. Its
equality demonstrates stable encoding for that workload; it does not establish
D2 for arbitrary callbacks or floating-point kernels.

## Legacy boundary

`SimCore::saveFrame()`, the demo snapshot wrapper, and
`SnapshotWriter`/`SnapshotReader` remain compatibility surfaces outside the
target M7 format. The legacy reader now bounds every access and refuses an
encoded vector length that does not fit the supplied input, and the demo
enforces a 64 MiB file policy. Those changes prevent the prior out-of-bounds
and attacker-sized allocation paths, but legacy native-layout artifacts are
not portable checkpoint-v1 artifacts and do not inherit the M7 D1 claim.

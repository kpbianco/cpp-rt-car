# Determinism & Replay

## Frame Snapshots
- Serialize Entity-Component-System (ECS) data, pseudo-random number generator (PRNG) states and phase graph state in a fixed order.
- Provide optional delta snapshots to reduce save/load bandwidth when taking many consecutive snapshots.

## Rollback/Fast-Forward Harness
- Given a snapshot, deterministically re-simulate `k` frames and compute a hash of the resulting state.
- Use the harness to validate determinism by comparing hashes after rollback/fast-forward operations.

## Counter-based PRNG
- Each entity or chunk uses an independent counter-based generator (e.g. Philox or Threefry).
- Seed protocol: combine a global seed with entity/chunk identifiers to guarantee reproducible streams.

This document outlines the minimal requirements for deterministic gameplay and replay within the simulation core.

## Code anchors

- Frame snapshots: `SimCore::saveFrame`, `SimCore::loadFrame`; `include/simcore/SimCore.hpp`
- Snapshot serialization helpers: `rt::SnapshotWriter::writeVector`, `rt::SnapshotWriter::writeMap`; `rt/include/rt/snapshot.hpp`
- Rollback harness: `TEST(SnapshotRollback, ReplayHashEquality)`; `tests/test_snapshot.cpp`
- Counter-based PRNG: `SimCore::prng`, `rt::prng::philox4x32_10`; `include/simcore/SimCore.hpp`, `rt/include/rt/prng.hpp`


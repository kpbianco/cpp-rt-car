# Concurrency Pattern Experiments

The repository includes independent examples of:

- `simcore::SeqLock<T>` for copyable read-mostly state;
- `simcore::LockFreeQueue<T>` with hazard-pointer reclamation;
- `simcore::TokenBucket` for rate-based admission.

They have focused tests, but they are not automatically used by `SimCore` and
do not by themselves satisfy the runtime memory, progress, or overload
contract. In particular, “lock-free” is an algorithmic progress property, not
a bound on completion time or allocation, and token-bucket admission is not a
queue-capacity plan.

Before a primitive enters an RT lane it needs explicit supported types,
allocation/reclamation ownership, memory-order reasoning, capacity behavior,
shutdown semantics, and contention tests.

## Code anchors

- Seqlock: `include/simcore/seqlock.hpp`
- Hazard-pointer queue: `include/simcore/lockfree_queue.hpp`
- Token bucket: `include/simcore/backpressure.hpp`

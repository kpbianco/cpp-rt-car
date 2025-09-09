# Concurrency Patterns

This project now includes additional patterns for highly concurrent systems:

## SeqLock

`simcore::SeqLock<T>` provides a lightweight seqlock for read-mostly state
such as configuration snapshots. Writers publish a new copy while readers
retry until a consistent sequence is observed.

## Hazard-pointer Queue

`simcore::LockFreeQueue<T>` implements a Michael & Scott queue using hazard
pointers for memory reclamation. It allows multiple producers and consumers
without locks.

## Back-pressure Token Bucket

`simcore::TokenBucket` implements admission control and pacing via a token
bucket. Each subsystem can meter work by acquiring tokens and relying on
rate-based refills to avoid burst avalanches.

Refer to the unit tests for usage examples.


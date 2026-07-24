# Determinism and Replay Status

The determinism tiers and evidence rules are normative in the
[product contract](product_contract.md). RTFW 0.7 exercises pieces of D1
(same-build reproducibility), but does not qualify D2 or D3.

## Current mechanisms

- deterministic range reduction uses fixed leaf partitioning, ordered
  combination, and per-leaf checksums;
- a counter-based Philox helper derives values from a seed and counter rather
  than task execution order;
- floating-point environment helpers select round-to-nearest and, on supported
  x86 builds, FTZ/DAZ;
- explicit `rt::fma` calls consult a process-global FMA switch;
- `SimCore::saveFrame()` serializes selected scheduler/runtime state in a fixed
  field order;
- integration tests compare representative state/hash results across selected
  worker counts within a build.

## Known gaps

- callbacks can use arbitrary mutable state, unordered iteration, clocks,
  random sources, and floating-point operations outside runtime control;
- the legacy `SimCore` FMA switch and trace pointer are process-global; the M1
  host runtime's numerical helper and trace are instance-local;
- floating-point behavior is not proven identical across compilers, ISAs, or
  machines;
- the current snapshot is native-layout data without a stable endian/width
  interchange contract;
- snapshot readers trust encoded lengths enough to attempt allocation before a
  complete size policy is applied;
- graph/config identity and user-owned simulation state are not represented by
  a generic schema;
- the demo's snapshot wrapper is specific to the example.

Milestone M7 adds a versioned, bounds-checked snapshot format, graph/config
identity, fuzzing, and tier-specific replay matrices. A D1 claim will apply only
to registered state and approved callbacks under a declared configuration.

## Code anchors

- Deterministic reduction: `include/simcore/deterministic_reduce.hpp`
- Counter PRNG: `rt/include/rt/prng.hpp`
- Numerical environment: `rt/include/rt/numerics.hpp`
- Runtime snapshot: `SimCore::saveFrame`, `SimCore::loadFrame`;
  `include/simcore/SimCore.hpp`
- Low-level codec: `rt/include/rt/snapshot.hpp`
- Current integration matrix: `tests/integration/test_determinism.cpp`

# Numerics Status

The repository provides numerical building blocks; it does not make arbitrary
user kernels deterministic.

## Available utilities

- `robust::sum_fp32_to_fp64` promotes FP32 inputs for accumulation.
- `robust::kahan_sum_fp32` provides compensated summation.
- `robust::ulp_distance` supports error-oriented tests.
- deterministic reduction helpers use a stable leaf/combine order.
- `rt::Q16_16` is a small fixed-point utility with a narrower, more explicit
  behavior surface than floating point.
- `rt::init_fp_env` requests round-to-nearest and enables FTZ/DAZ on supported
  x86 threads.
- `rt::fma` wraps an explicitly gated fused multiply-add.
- M1 callback contexts expose an instance-local `NumericalPolicy` whose
  `multiply_add` operation is selected before finalization.

## Limits

The legacy `rt::fma` setting used by `SimCore` is process-global. It only
controls calls made through `rt::fma`; compilers may contract or transform
other floating-point expressions according to build flags. The M1
`rt::Runtime` does not modify that flag: its precise/FMA choice applies only to
the numerical helper passed to its callbacks and is isolated per instance.
FTZ/DAZ is thread-local and must be applied to every future execution context,
including host and device callbacks.

Different compilers, standard libraries, ISAs, math libraries, and input
handling can still produce different bits. The current tests support only the
limited D1 statement in the [product contract](product_contract.md).

## Code anchors

- Mixed-precision and ULP helpers: `include/simcore/robust_fp.hpp`
- Reduction order: `include/simcore/deterministic_reduce.hpp`
- FP environment/FMA wrapper: `rt/include/rt/numerics.hpp`
- Instance-local callback numerical helper: `rt/include/rt/runtime.hpp`,
  `rt/src/host_runtime.cpp`
- Fixed point: `rt/include/rt/fixed_point.hpp`

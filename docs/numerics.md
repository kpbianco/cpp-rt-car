# Numerics & Determinism

This project treats numerical determinism as a first-class goal.

## Mixed-Precision Reductions

`robust::sum_fp32_to_fp64` explicitly promotes single-precision streams
to double precision for reductions. For sensitive paths,
`robust::kahan_sum_fp32` provides compensated summation.

## Shadow Arithmetic

Tests use double precision "shadow" values and
`robust::ulp_distance` to track error growth and detect unstable
kernels.

## Denormals & FMA Control

`rt::init_fp_env` sets FE_TONEAREST and enables flush-to-zero and
denormals-are-zero per thread. `rt::set_use_fma` gates fused
multiply-add so behaviour is deterministic across targets.

## Fixed-Point Option

For hard real-time subsystems a small fixed-point type `rt::Q16_16`
implements deterministic arithmetic without relying on floating point
hardware.


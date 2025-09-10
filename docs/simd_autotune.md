# SIMD AoSoA Autotuning

The SIMD primitives now provide a simple runtime autotuner for Array-of-Structs-of-Arrays (AoSoA) block sizes. The
`soa::autotune_block_size` helper benchmarks candidate tile sizes (64, 128, 256 and 512) using CPU cycle counters and
returns the fastest option for the requested problem size. This allows experiments to adapt to the host's micro-architecture
without manual tuning.

Additionally the AoSoA `axpy3` kernel has been updated to operate on tails padded to the SIMD width. Padding avoids
mask loads/stores on the final block which simplifies the generated code.

Manual prefetching is now guarded by a small working-set heuristic. Prefetch instructions are only issued when the
array length exceeds a conservative threshold (`PREFETCH_MIN_N`), ensuring small problems do not pay the overhead of
prefetching.

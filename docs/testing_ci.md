# Testing and CI Beyond Correctness

This project goes beyond traditional unit tests.  The following
strategies are used to harden the runtime and catch regressions early.

## Differential Tests

`tests/test_differential_output.cpp` loads kernel results and compares
them against a baseline stored under `tests/golden/`.  The test computes
the maximum absolute drift and fails when it exceeds a threshold.  This
keeps refactors honest when small numerical deltas could mask behavioural
regressions.  The check runs automatically under the GoogleTest suite in
CI.

## Chaos Tests

Fault injection and other chaos-style checks live alongside the normal
unit tests.  For example `tests/test_fault_injection.cpp` exercises
allocator failures and other transient errors.  Similar patterns can be
extended to simulate stalls or thread preemption storms.

## Performance CI Stability

Benchmarks should be executed multiple times and evaluated using robust
statistics (e.g. Hodges–Lehmann estimator) rather than a single sample.
CI gates on the confidence interval of these runs instead of raw
wall-clock timings.

## Fuzzing and Sanitizers

LibFuzzer harnesses (e.g. `tests/jobqueue_fuzz.cpp`) run under
ASan/UBSan/TSan.  Any TSan suppressions must carry written justification
within the repository.

## Code anchors

- Differential testing: `TEST(DifferentialKernelTest, DriftBelowThreshold)`; `tests/test_differential_output.cpp`
- Chaos testing: `TEST(FaultInjection, RandomDelaysStillRun)`; `tests/test_fault_injection.cpp`
- Fuzzing harness: `LLVMFuzzerTestOneInput`; `tests/jobqueue_fuzz.cpp`


# Testing and CI Beyond Correctness

This project goes beyond traditional unit tests.  The following
strategies are used to harden the runtime and catch regressions early.

## Differential Tests

`tools/differential_test.py` compares kernel outputs against a known
baseline and fails when the maximum absolute drift exceeds a threshold.
This is useful when refactoring complex kernels where small numerical
changes may hide large behavioural regressions.

Example usage:

```bash
python tools/differential_test.py old.txt new.txt --threshold 0.01
```

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


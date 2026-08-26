# Sanitizer Builds

`SIM_SANITIZERS` passes a comma-joined sanitizer list to supported GCC/Clang
compile and link commands. Use compatible families in separate build
directories; AddressSanitizer and ThreadSanitizer cannot be combined.

Address/undefined example:

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TESTS=ON \
  -DSIM_SANITIZERS="address;undefined"
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure
```

ThreadSanitizer example:

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TESTS=ON \
  -DSIM_SANITIZERS=thread
cmake --build build-tsan --parallel 2
ctest --test-dir build-tsan --output-on-failure
```

Compiler/platform support varies for `address`, `undefined`, `leak`, `memory`,
`thread`, `hwaddress`, `safe-stack`, and `cfi`. MemorySanitizer normally
requires an instrumented C++ runtime; CFI requires compatible LTO and visibility
settings. A successful configure does not imply every requested combination is
supported.

The optional `SIM_BUILD_FUZZERS=ON` targets use Clang libFuzzer with
address/undefined instrumentation. M20-PRE-01 runs the supported
checkpoint/input-log and runtime-profile harnesses with 64 KiB input ceilings
and 20,000 fixed-seed mutations each. The experimental job-queue harness has a
4 KiB ceiling and 10,000 mutations. Every named immutable seed is replayed
first, and failure artifacts stay below the explicit build output. These are
bounded deterministic smoke tests, not continuous fuzzing or proof of
vulnerability absence.

## Code anchors

- Global sanitizer flags: `CMakeLists.txt`
- Optional fuzz targets and runner: `tests/CMakeLists.txt`,
  `tests/snapshot_fuzz.cpp`, `tests/runtime_profile_fuzz.cpp`,
  `tests/jobqueue_fuzz.cpp`, `tests/fuzz/`, and
  `tools/run_fuzz_smoke.py`

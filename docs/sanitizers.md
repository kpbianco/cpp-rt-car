# Sanitizer Support

This project can be built with a variety of LLVM sanitizers to help debug
memory and concurrency issues before deploying to real hardware.

## Enabling

Pass a semicolon-separated list via `SIM_SANITIZERS` when configuring CMake:

```bash
cmake -B build -DSIM_SANITIZERS="address;thread" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Sanitizers are available when using Clang or GCC. Supported entries include:

- `address` – AddressSanitizer for buffer overflows and use-after-free.
- `leak` – LeakSanitizer for detecting memory leaks.
- `memory` – MemorySanitizer for uninitialized reads (clang only).
- `thread` – ThreadSanitizer for data races.
- `hwaddress` – HWAddressSanitizer for tagged memory (AArch64).
- `safe-stack` – SafeStack for protecting the return stack.
- `cfi` – Control Flow Integrity (requires LTO).

Multiple sanitizers may be combined when they are compatible.

# Contributor & Operations Guide

## Development
1. Configure the project:
   ```bash
   cmake -B build -S . -DENABLE_TESTS=ON
   ```
2. Build and run tests:
   ```bash
   cmake --build build -j
   (cd build && ctest --output-on-failure)
   ```

## Style
- Follow C++20 core guidelines.
- Run all tests before submitting a PR.

## Operations
- Logs and profiles can be toggled with `ENABLE_LOG` and `ENABLE_PROF` in CMake.
- For sanitizer builds, use `-DSIM_SANITIZERS=address;undefined`.

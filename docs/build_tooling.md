# Build and Tooling Guide

## Direct build

The supported baseline is CMake 3.20+ and C++20:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Tests use the checked-in GoogleTest submodule by default. Clone submodules
recursively or initialize them before configuring with tests.

## Optional CUDA Driver API adapter

The bounded CUDA state machine (`rtfw::cuda_backend`) is built without a
toolkit dependency so CPU-only CI can test it. Enable the production adapter
only when CUDAToolkit and a compatible driver environment are available:

```bash
cmake -S . -B build-cuda \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON \
  -DRTFW_ENABLE_CUDA=ON
cmake --build build-cuda \
  --target simcore_tests sample_cuda_qualification \
  --parallel 2
GTEST_FILTER='CudaBackend.*' \
  ctest --test-dir build-cuda --output-on-failure -R simcore_all
```

Installed CUDA-enabled packages export `rtfw::cuda_driver`, propagate the
`RTFW_CUDA_DRIVER_AVAILABLE` definition, and resolve `CUDA::cuda_driver`.
Packages built with the default `RTFW_ENABLE_CUDA=OFF` still export
`rtfw::cuda_backend` for injection/testing but do not expose the production
adapter symbol. See [the CUDA contract](cuda_backend.md).

## Optional Xilinx XDMA Linux adapter

The portable fixed-capacity state machine is always available as
`rtfw::xdma_backend`. On Linux, enable the production adapter and destructive
AXI-MM qualification tool explicitly:

```bash
cmake -S . -B build-xdma \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON \
  -DRTFW_ENABLE_XDMA=ON
cmake --build build-xdma \
  --target rtfw_xdma_backend_tests sample_xdma_qualification \
  --parallel 2
ctest --test-dir build-xdma --output-on-failure -R xdma_backend
```

The host must separately install/load the official Xilinx XDMA Linux driver
and program the declared AXI-MM-compatible bitstream. The library does not
install a kernel module, mutate driver policy, or discover a safe device range.
See [the XDMA contract](xdma_backend.md) before running the qualification tool.

## Presets

The Ninja presets write under `build/`:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure

cmake --preset release
cmake --build --preset release
```

The Release preset requests LTO/ThinLTO. Compiler and generator support is
checked during configuration.

## Experimental PGO

`pgo-generate` and `pgo-use` expose compiler profile flags, but the repository
does not orchestrate a representative training workload, merge profiles across
processes, or validate that a profile matches the binary. Treat these presets
as scaffolding:

```bash
cmake --preset pgo-generate
cmake --build --preset pgo-generate
# Run a deliberately selected training workload here.
cmake --preset pgo-use
cmake --build --preset pgo-use
```

## Optional analysis

- `SIM_ENABLE_IWYU=ON` requests include-what-you-use when the executable is
  installed.
- `SIM_WARN_ODR=ON` adds supported ODR warnings.
- a `bloaty` target is created only when Bloaty is found.
- `SIM_ENABLE_AVX2=ON` applies global x86 AVX2/FMA compile flags and is
  experimental; it is not runtime ISA dispatch.
- sanitizer combinations are described in [sanitizers](sanitizers.md).

## Installation/versioning

`VERSION.txt` is the release source of truth. CMake installs shared/static
libraries, public headers, package config/version files, the license, and the
version file. A clean external `find_package(rtfw CONFIG)` consumer remains an
M11 release gate.

## Code anchors

- Build configuration and installation: `CMakeLists.txt`
- Presets: `CMakePresets.json`
- Clang toolchain: `cmake/toolchains/clang.cmake`
- Version source: `VERSION.txt`

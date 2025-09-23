# Build and Tooling Guide

This project supports a number of build helpers to aid in size and
performance tuning.

## CMake presets and toolchains

Use the provided `CMakePresets.json` for common configurations:

```
cmake --preset dev      # Debug build with tests
cmake --preset release  # Release build with ThinLTO
cmake --preset pgo-generate  # Instrumentation phase
cmake --preset pgo-use       # Optimisation phase using collected profile
```

A simple Clang toolchain is available in `cmake/toolchains/clang.cmake`.

## PGO and ThinLTO

PGO can be executed in two phases. The presets above store profiles under
`profiles/` allowing separate generation and use steps.

## Size analysis with Bloaty

If [Bloaty](https://github.com/google/bloaty) is installed a `bloaty`
custom target is generated. Invoke it after building to inspect code bloat:

```
cmake --build build/release
cmake --build build/release --target bloaty
```

## Header hygiene

Enable include-what-you-use and ODR warnings by toggling the options:

```
-DSIM_ENABLE_IWYU=ON -DSIM_WARN_ODR=ON
```

These checks help keep headers minimal and catch One Definition Rule
violations early.

## Code anchors

- Presets and PGO flow: `configurePresets` entries (`dev`, `release`, `pgo-generate`, `pgo-use`); `CMakePresets.json`, `CMakeLists.txt` (`SIM_PGO`, `SIM_ENABLE_LTO`)
- Clang toolchain: `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`; `cmake/toolchains/clang.cmake`
- Bloaty analysis: `add_custom_target(bloaty)`; `CMakeLists.txt`
- Header hygiene toggles: `option(SIM_ENABLE_IWYU)`, `option(SIM_WARN_ODR)`; `CMakeLists.txt`


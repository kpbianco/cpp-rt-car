# Developer Experience and Governance

This project provides a lightweight template for adding new systems via the
`tools/new_system.py` generator. Consistent coding style and comprehensive
testing are required for all contributions.

Plugins must adhere to the C ABI defined in `rt/plugin_api.h` and carry strict
`abi_major` and `abi_minor` version numbers. Breaking changes require bumping
the major version.

## Contribution Workflow

1. Create a feature branch and implement the change.
2. Run `cmake` and `ctest` before submitting a pull request.
3. Ensure new code has unit tests and documentation.
4. Follow semantic versioning for public interfaces, including plugins.

## Governance

Changes are reviewed by maintainers. A quorum of two maintainers must approve
API or ABI changes.

## Scheduling Invariants

* Development builds define `RTFW_ENFORCE_NO_RAW_THREADS`. When this flag is
  active, including `SimCore.hpp` forbids direct `std::thread` construction.
  Phases must submit asynchronous work through the provided worker pool to keep
  a single scheduling surface and maintain consistent telemetry.

## Code anchors

- System scaffolding: `main()` generator; `tools/new_system.py`
- Plugin ABI contract: `RT_PLUGIN_API_VERSION_MAJOR`, `rt_plugin_desc_t`; `rt/include/rt/plugin_api.h`
- Raw thread guard: `simcore::debug::assert_thread_creation_allowed`, `#define thread(...)`; `include/simcore/debug.hpp`, `include/simcore/SimCore.hpp`


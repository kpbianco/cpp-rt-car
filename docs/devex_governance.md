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

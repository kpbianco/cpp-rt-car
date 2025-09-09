# Security and Supply Chain

This project includes a small collection of facilities aimed at
improving the security posture and supply‑chain hygiene of the runtime
and its tooling.

## Sandboxing

The header `simcore/sandbox.hpp` exposes `enable_sandbox()` which
restricts the current process. On Linux it installs a minimal `seccomp`
filter; on Windows a Job Object is created so that tools terminate if
the parent process exits.

## Minidump symbol support

`write_minidump()` now accepts an optional list of symbol paths. These
are embedded in the dump file so that a private symbol server can be
consulted when resolving crash reports.

## SBOM and signature verification

`tools/sbom.py` produces a JSON Software Bill of Materials describing
pinned git submodules. When provided with `tools/sbom_expected.json`
the script verifies that the repository is using known commits for its
third‑party dependencies.

## Reproducible build artifacts

`tools/store_repro_build.py` stores build outputs along with a SHA256
digest.  The resulting metadata can be kept with performance artifacts
allowing binary reproducibility checks.

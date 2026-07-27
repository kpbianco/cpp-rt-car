# Security and Supply-Chain Status

RTFW 0.12 has security-oriented experiments and CI helpers, not a hardened
plugin sandbox or complete supply-chain policy.

## Process restriction experiment

On Linux, `enable_sandbox()` attempts to install a very small seccomp allowlist
for read, write, exit, exit-group, and signal return. Such a policy applies to
the current process and is too narrow for the runtime's normal thread, memory,
clock, file, and device behavior. It is not invoked by the runtime lifecycle
and must be treated as an isolated test helper.

On Windows, the helper assigns the current process to a Job Object configured
to terminate with the job. That is lifecycle containment, not a security
sandbox or plugin isolation boundary.

Plugins run in process and therefore share the host's authority. Only trusted
plugins should be loaded.

## Crash text helper

`write_minidump()` writes symbol-path strings and, on Unix-like systems, a text
backtrace. It is not a platform minidump format, is not async-signal-safe, and
may disclose paths. It must not be described as a production crash handler.

## Supply-chain helpers

- CI can generate an SPDX JSON SBOM with `anchore/sbom-action`.
- `tools/sbom.py` inventories git submodules and can compare them with a
  repository allowlist.
- `tools/store_repro_build.py` records artifact hashes and metadata.

These are useful inputs, but signing, provenance attestation, release-key
policy, vulnerability response, and reproducible-build verification remain
release-engineering work.

## Code anchors

- Process restriction experiment: `include/simcore/sandbox.hpp`
- Crash text helper: `include/simcore/minidump.hpp`
- Submodule SBOM check: `tools/sbom.py`
- Artifact metadata: `tools/store_repro_build.py`
- Threat boundaries: [threat model](threat_model.md)


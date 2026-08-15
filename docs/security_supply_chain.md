# Security and Supply-Chain Status

RTFW 1.2 has a portable release-integrity contract and security-oriented CI
helpers. It is not a hardened plugin sandbox and does not claim signed
provenance or reproducible builds.

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
- `tools/release_manifest.py` records and verifies every packaged artifact's
  relative path, byte length, SHA-256 digest, release version, and complete
  source commit.
- `tools/stage_release_artifacts.py` rejects stale or unexpected top-level
  CPack output and admits only one archive with a matching checksum sidecar.
- `tools/extract_release_archive.py` applies member-count/expanded-size bounds
  and rejects traversal, duplicates, unsafe links, and unsupported archive
  member types before the packaged-consumer gate.
- `release/rtfw-release-contract.json` locks reviewed portable support,
  compatibility, ABI, and public-contract digests.
- Every workflow action is pinned to a reviewed full commit rather than a
  movable major-version tag.
- `tools/store_repro_build.py` records content-addressed build artifacts and
  metadata for comparison.
- `tools/qualification.py` bounds strict JSON and artifact-tree input, rejects
  duplicate keys, non-finite numbers, unsafe paths, symlinks, unlisted files,
  and digest drift, and publishes a canonical proposal only to a nonexisting
  explicit output. Its SHA-256 chains are integrity metadata, not signatures,
  reviewer authentication, or proof of plan chronology.

The archive manifest detects accidental corruption or substitution after it is
created, but it is not authentication. Signing, provenance attestation,
release-key policy, and reproducible-build verification remain release-
engineering work. Vulnerability reporting and supported security versions are
defined in the repository [security policy](../SECURITY.md).

## Code anchors

- Process restriction experiment: `include/simcore/sandbox.hpp`
- Crash text helper: `include/simcore/minidump.hpp`
- Submodule SBOM check: `tools/sbom.py`
- Release contract and artifact metadata:
  `tools/check_release_contract.py`, `tools/release_manifest.py`,
  `tools/stage_release_artifacts.py`, `tools/extract_release_archive.py`,
  `tools/store_repro_build.py`
- Qualification input and proposal safety: `tools/qualification.py`,
  `qualification/schemas/`, and [qualification contract](qualification.md)
- Threat boundaries: [threat model](threat_model.md)
# Trusted in-process extensions

Extension ABI validation is not a sandbox or authentication mechanism. The
embedding host owns artifact provenance, trust policy, symbol resolution,
module lifetime, and unload. Runtime never searches or loads a path. Unknown
suffix bytes are ignored, known reserved fields must be zero, and malformed or
incompatible records fail before publication. A successful fixture or unload
readiness check does not establish safe arbitrary native code.

# Unreal source integration

The M19-02 plugin is source only and requires separately licensed access to the
exact Unreal checkout. No engine source or binary is vendored or redistributed
by RTFW. `RTFW_UNREAL_ENGINE_ROOT` and `RTFW_UNREAL_SDK_ROOT` are trusted build
inputs, not authentication boundaries. The governed verifier checks the engine
commit/tag/version, bundled compiler/linker identity, runtime-archive compiler
comment, and retains the archive digest; the CI job builds that archive from
the checked-out target revision. A caller that bypasses the verifier can link
arbitrary native code into the engine process.

The plugin uses only public engine headers except for the exact D-009
`LowLevelTasks::FTask`/`TryLaunch` source exception. It adds no download,
network, credential, loader, path-search, or dynamic-module operation. Engine
license approval, runner custody, and post-run artifact access remain human
controls. A digest or exact commit proves identity/integrity only, not safety,
absence of engine-internal allocation, or signed provenance.

# Changelog

All notable changes to the supported target runtime are recorded here. The
project follows [Semantic Versioning](https://semver.org/) for package releases;
the stable C ABI has its own version.

## 1.2.1

Recoverable device lifecycle safety:

- preserves failed buffer-unregistration and backend-shutdown ownership in
  deterministic reverse order instead of discarding backend status;
- makes `stop()` idempotently retry only unresolved device cleanup, keeps the
  prior public lifecycle state on failure, and gates execution and state
  mutation until teardown succeeds;
- treats failed backend initialization as ownership-uncertain and performs a
  checked shutdown pass, retaining the backend for later retry when that
  cleanup also fails;
- keeps the ABI-v8 `rtfw_destroy` handle alive when its implicit stop cannot
  release borrowed device resources, allowing a caller that followed the
  checked-stop contract to retry;
- adds injected multi-backend, failed-start, state-mutation, CUDA/XDMA
  partial-init, and dynamic-C teardown recovery coverage;
- retains stable C ABI v8 with 70 exports, device ABI v1, all schema versions,
  SONAME 8, the M14 SDK inventory, and the unmodified Apache-2.0 license.

## 1.2.0

Professional SDK and package boundary:

- adds `rtfw::runtime` and the `runtime` package component as the preferred
  supported C++20 integration surface while retaining `cpp_runtime` and
  `rtfw::simcore_rt` as 1.x compatibility names;
- splits the supported runtime archive from plugin, scheduler, crashdump,
  fiber, detached GPU-stub, and legacy SimCore implementation paths;
- installs an exact default public-header allowlist and keeps broader research
  headers/targets behind explicit build/install options;
- adds focused status/configuration/canonical-byte entry points while
  retaining `profile.hpp`'s 1.x transitive runtime include for compatibility;
- prevents warning/Werror, logging, profiler, and legacy target dependencies
  from leaking into installed consumers and propagates the required C++20
  feature from `rtfw::runtime`;
- isolates optional CUDA/XDMA adapter exports so ordinary runtime and C
  consumers do not acquire vendor dependencies;
- makes `add_subdirectory` library-only by default, preserves parent test and
  CPack ownership, and honors non-default install include/data directories;
- adds relocated header self-containment, exact package inventory, genuinely
  C-only shared/static consumers, preferred and compatibility target,
  independent backend, policy-leak, and installed Apache-2.0 digest gates;
- retains stable C ABI v8, device ABI v1, schema versions, SONAME 8, and the
  unmodified Apache-2.0 license.

## 1.1.0

Runtime-profile and autotune integration:

- adds installed `<rt/profile.hpp>` with a bounded, allocation-free,
  transactional parser for complete runtime-config schema-v7 JSON profiles;
- rejects malformed UTF-8/JSON, oversized/nested input, unknown or duplicate
  contract keys, missing fields, invalid cross-field values, incompatible
  profile/config/package versions, and trailing bytes without mutating output;
- adds `rtfw_runtime_demo`, a profile-driven `rt::Runtime` workload with four
  independent physics systems, a dependency barrier, nested CPU work, finite
  host/self-paced runs, and direct frame/executor metrics;
- restricts the production autotune factor space to consumed target-runtime
  fields and makes generated-profile C++ round-trip testing mandatory in CI;
- replaces inert legacy config fixtures with complete versioned profiles and
  content-derived profile IDs;
- retains stable C ABI v8, device ABI behavior, portable RT0 qualification
  boundaries, and the unmodified Apache-2.0 license.

## 1.0.0

Portable RT0 release:

- declares named GCC, Clang, and MSVC build/test tuples and a 1.x support
  policy;
- closes the multi-runtime device-state isolation gate;
- adds checked release-contract, strict CPack artifact staging, and
  content-addressed package/evidence-manifest tooling;
- adds exact-archive extraction and consumer gates for the supported Linux and
  Windows tuples;
- adds all-tuple tag publication after every archive consumer succeeds;
- pins every third-party workflow action to a reviewed commit;
- retains stable C ABI v8 and same-major CMake package compatibility;
- keeps PREEMPT_RT, CUDA, and XDMA qualification separate and unclaimed.

The supported product is the bounded `rt::Runtime` path. Legacy `SimCore`,
plugin, scheduler, fiber, and utility experiments remain outside the 1.x
compatibility contract.

## 0.12.0

- Froze C ABI v8 as the first stable binary boundary.
- Added exact exported-symbol and header-fingerprint checks.
- Added relocated shared-C, static-C, and C++ package consumers.
- Added the bounded host job-system executor adapter.

## 0.2.0–0.11.0

The pre-1.0 milestones introduced lifecycle/configuration, compiled graphs, the
unified executor, memory closure, absolute periodic time, observability,
checkpoint/replay, the device ABI/mock, and CUDA/XDMA backend candidates. See
[the completion roadmap](docs/roadmap.md) for their detailed exit gates.

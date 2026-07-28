# Changelog

All notable changes to the supported target runtime are recorded here. The
project follows [Semantic Versioning](https://semver.org/) for package releases;
the stable C ABI has its own version.

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

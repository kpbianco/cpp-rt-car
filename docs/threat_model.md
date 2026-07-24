# Threat Model

## Trust boundary

RTFW 0.8 is an in-process native library prototype. The host application,
runtime, callbacks, and loaded plugins share one address space and authority.
Plugins and device backends are trusted code; ABI validation is not a security
boundary.

## Assets

- host process memory and simulation state;
- deadlines and service availability;
- snapshot/config/profile integrity;
- device buffers and command streams;
- trace, log, crash, and experiment artifacts;
- build and dependency provenance.

## Principal threats and current posture

| Threat | Current posture |
| --- | --- |
| Malicious/buggy callback or plugin | Full in-process access; version fields catch some incompatibility only |
| Malformed graph | M2 rejects invalid/foreign handles, cycles, duplicate declarations, and unordered declared resource conflicts before start; omitted resource declarations remain host error |
| Queue/memory exhaustion | Target `rt::Runtime` has a budgeted plan and bounded queue/task-scratch rejection; legacy arenas, workers, and device experiments still contain spin, fallback-allocation, detached-helper, or unchecked paths |
| Malformed checkpoint/config | M1 runtime keys and C structure headers fail closed; M7 target checkpoints/input logs have absolute bounds, schema/identity checks, transactional restore, and fuzz coverage, but their FNV checksums are not authentication; legacy snapshots, profiles, and other experimental parsers remain outside that contract |
| Device hang/loss | CPU mock only; no bounded backend reset/health contract |
| Telemetry leakage/corruption | M6 target records are size/schema tagged with cursor loss accounting, but non-RT exporters can disclose runtime data and no sink access policy exists; legacy telemetry remains experimental |
| Supply-chain substitution | Submodule/SBOM helpers and CI dependency review exist; signed release provenance is not established |
| Host-policy mutation | Hardening scripts can make privileged system-wide changes and require operator review; M5 strict preflight is read-only |

## Required controls

Before stable release, the project needs bounds and resource ceilings for every
remaining untrusted-input parser, backend timeouts/reset, signed release
provenance, exporter access policy, and a documented trusted-plugin policy.
Untrusted extensions require an out-of-process boundary; the tiny
`enable_sandbox()` experiment is not sufficient.

See [security status](security_supply_chain.md) and the
[product contract](product_contract.md).

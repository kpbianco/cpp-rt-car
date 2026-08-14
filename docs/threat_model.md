# Threat Model

## Trust boundary

RTFW 1.2 is an in-process native library. The host application, runtime,
callbacks, and loaded plugins share one address space and authority. Plugins
and device backends are trusted code; ABI validation is not a security
boundary. Portable RT0 support is not a sandboxing claim.

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
| Queue/memory exhaustion | Target `rt::Runtime` has a budgeted CPU/device plan and bounded queue, task-scratch, outstanding-slot, and backend rejection; legacy arenas, workers, and the detached GPU stub retain weaker paths |
| Malformed checkpoint/config/profile | M1 runtime keys and C structure headers fail closed; M7 target checkpoints/input logs have absolute bounds, schema/identity checks, transactional restore, and fuzz coverage, but their FNV checksums are not authentication; M13 profiles are limited to 64 KiB/16 levels, validate UTF-8/JSON plus exact contract fields and compatibility, parse without allocation, and update outputs transactionally; profile IDs are provenance, not authentication; legacy snapshots and other experimental parsers remain outside that contract |
| Device hang/loss | M8 defines timeout/loss statuses, poll-only completion, health/reset/shutdown, and deterministic mock evidence; M9 quarantines timed-out or uncertain CUDA work until physical drain and treats context loss as host-recreation-required, but a malicious backend still shares process authority and no hardware-driver recovery tuple is qualified |
| Telemetry leakage/corruption | M6 target records are size/schema tagged with cursor loss accounting, but non-RT exporters can disclose runtime data and no sink access policy exists; legacy telemetry remains experimental |
| Supply-chain substitution | Submodule/SBOM helpers, CI dependency review, a checked release contract, and complete artifact SHA-256 manifests exist; manifests are unsigned and signed provenance is not established |
| Malformed qualification evidence | M18-01 applies byte, depth, count, numeric, duplicate-key, strict-UTF-8, closed-object, canonical-path, symlink, regular-file, inventory, size, and digest checks before proposal output. Hashes do not authenticate the reviewer, prove that the plan predates measurement, or make untrusted native artifacts safe to execute. |
| Host-policy mutation | Hardening scripts can make privileged system-wide changes and require operator review; M5 strict preflight is read-only |

## Required controls

Future hardening needs continuous mutation/fuzz coverage for the M13 profile
parser, bounds and resource ceilings for every remaining untrusted-input
parser, backend timeouts/reset, signed release
provenance, exporter access policy, and a documented trusted-plugin policy.
Untrusted extensions require an out-of-process boundary; the tiny
`enable_sandbox()` experiment is not sufficient.

See [security status](security_supply_chain.md) and the
[product contract](product_contract.md).
# M19-01 extension threats

An extension executes trusted native code in-process and can corrupt the host;
ABI checks do not create isolation. Mitigated integration hazards include
short/forward records, incompatible ranges, poisoned partial transactions,
forged or stale handles, exceptions crossing callbacks, service/backend
teardown ordering, and premature callable clearing. The host must authenticate
code and retain it until checked detach. Runtime has no loader, path search,
module handle, sandbox, or hot-reload orchestration.

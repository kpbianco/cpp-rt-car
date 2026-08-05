# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. The exact M17-01 baseline is
`e264278557866acbf3bda1e6b4a436a6e0fd704f`, the merged M16-04 commit. M14,
M14.1, M15, and M16 are complete in target history. M17 is active and remains
incomplete. `contracts/active-batch.yaml` is binding.

Canonical control artifacts are under
`/home/kbianco/.local/share/portfolio-control/worktrees/control/cpp-rt-car/products/cpp-rt-car`
at revision `7726a89440df85ce9ff4b2e4af77deb173b9fda2`.

## Implemented boundary

M17-01 adds a distinct C++ HAL API version 2 core contract to the existing
installed `rt/device.hpp`; it adds no installed header or target. Public
records and the copied table cover the bounded core operations and keep the
64-byte backend ID, 128-byte inline payload, and eight buffer-reference limit.
The native registration overload is configuring-only and returns the same
instance-owned `DeviceBackendHandle` as the legacy registration.

Legacy `DeviceBackendRegistration` remains the device-ABI-v1 source surface.
Each accepted v1 table is copied once into a separately allocated,
Runtime-owned adapter whose address survives configuring-vector growth. The
device manager stores HAL v2 tables only. Direct v1 calls are confined to
`rt/src/hal_v2.cpp`; all native and adapted lifecycle, buffer, submit, poll,
health, reset, and shutdown behavior uses the canonical manager.

The adapter translates every shared field and known status without retry or
success promotion. `UNSUPPORTED` stays unsupported at the HAL boundary.
Unknown statuses, malformed sizes/versions/reserved fields, invalid enums or
booleans, unwritten or oversized outputs, and callback exceptions fail closed.
Tokens, counts, capabilities, health, and completions publish only after
complete validation.

The existing early-completion causal handshake and graph token ownership are
unchanged. Only the device-service lane polls. Checked stop quiesces graph and
device work, unregisters buffers in reverse order, shuts backends down in
reverse order, retains the first error, and retries only unresolved ownership.
No new lane, blocking mutex, spill path, callback from poll, or steady-state
allocation is introduced.

Adapter/table/context and v1 completion scratch are part of
`device_control_bytes` and the M15 device extent exactly once. The six planned
MemoryPlan rows and three provider-backed regions are unchanged. Adapted v1
uses the exact legacy graph/replay identity bytes; a native v2 backend adds a
conditional kind/API-version marker.

The deterministic mock, CUDA candidate, and XDMA candidate implementations are
unchanged and register through the v1 adapter. Their portable mock/fake-driver
suites are compatibility evidence, not physical-hardware qualification.

## Protected decisions

- Preserve C ABI v8, 70 exports/fingerprint, SONAME 8, and every device ABI v1
  declaration/layout/value.
- Preserve Runtime statuses, profile schema 7/25 keys, observability schema 2
  and IDs, checkpoint/input-log schema 1, and rate-action schema 1.
- Preserve installed headers/targets and aliases, support matrices, release
  1.2.1, and Apache-2.0.
- Do not add heterogeneous memory/topology/coherency/timestamps, batches,
  timelines, vendor-control facilities, peer memory, plugins/factories,
  device-rate execution, or another submission lane in M17-01.
- Do not claim hardware, HIL, field, controlled latency, RT1, RT2, signing,
  release, staging, deployment, or production evidence.

## Completion output

Run the exact local commands in `contracts/active-batch.yaml`, ending with
`./scripts/agent-verify.sh full`, C ABI verification, and the SONAME check.
Retain acceptance mapping, commands/results, storage/identity/lifecycle facts,
rollback, residual risks, and all unperformed validation in
`docs/evidence/M17-01-2026-08-05.md`. Mandatory CI and human review remain
external gates. Do not commit, push, open a pull request, release, or deploy
without separate authorization.

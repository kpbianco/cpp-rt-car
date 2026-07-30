# Agentic delivery gaps

## Closed by this harness

- Repository-local `AGENTS.md` now routes agents to exact contracts, invariants,
  commands, skills, and claim limits.
- The assurance profile and control-plane source are explicit.
- An active bounded M15-01 contract replaces ad hoc web prompts.
- Local contract, quick, and full verification have one fail-fast entry point
  with evidence logs.
- Current state and restart handoff are versioned in the repository.
- A project-specific skill defines stop conditions and evidence requirements.

## Immediate implementation gaps

1. M15-01 has not yet implemented the public policy/report model or exact
   thread/memory inventory.
2. Focused tests named by the active batch do not yet exist.
3. Batch evidence is not yet generated.
4. The local `full` profile is a single-host approximation; GitHub CI remains
   authoritative for the named compiler/OS, sanitizer, package, and
   determinism matrix.
5. Performance regression is not yet a required controlled-hardware gate.

## Milestone gaps

- **M15:** native per-role apply/verify, memory providers and residency, full
  accounting, and rollback.
- **M16:** rate compiler, cross-rate freshness, admission, late policy, and
  shedding/recovery.
- **M17:** HAL v2, heterogeneous memory, timeline completions, CUDA Graph, XDMA
  control/event facilities, and combined sample.
- **M18:** exact NVIDIA, XDMA, combined, RT1, optional RT2, endurance, thermal,
  loss/reset/rebind, and shutdown qualification.
- **M19:** size/versioned extension ABI, Unreal adapter, lifecycle/unload, and
  complete sample.
- **M20:** controlled performance gates, continuous fuzzing, broader static and
  security analysis, signed provenance, non-RT telemetry adapters, safe
  frame-boundary control updates, platform expansion, API reference, recipes,
  and migrations.

## CI-agent rollout gap

Do not add unattended Codex/GitHub Agentic Workflows in this scaffold. First use
the local harness for real M15 pull requests and measure:

- first-pass CI success;
- accepted review findings and false positives;
- diagnosis and repair success;
- API cost if CI agents are enabled later;
- escaped regressions and reverts.

A later approved PR may add read-oriented PR risk review and CI diagnosis.
Automated repair must remain maintainer-triggered, isolated, path-bounded, and
unable to merge or waive deterministic CI.

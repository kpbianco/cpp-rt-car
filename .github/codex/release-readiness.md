---
on:
  workflow_dispatch:

permissions: read-all
engine: codex

safe-outputs:
  create-issue:
---

# Release Readiness Assessment

Assess the current default-branch release candidate using `AGENTS.md`, the repository profile, acceptance contracts, CI results, release configuration, migrations, rollback procedures, runbooks, security findings, and available manual validation evidence.

Create one issue with a `go`, `conditional-go`, or `no-go` result. Include blockers, missing evidence, rollback readiness, observability readiness, and exact sources. Do not publish or deploy anything.

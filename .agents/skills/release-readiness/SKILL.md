---
name: release-readiness
description: Assess whether a release candidate satisfies acceptance, migration, rollback, security, observability, documentation, and validation requirements.
---

## Procedure
1. Confirm all required deterministic gates passed on the exact candidate.
2. Check versioning, migrations, compatibility, rollback, runbooks, telemetry, and unresolved high-risk findings.
3. Verify profile-specific manual gates such as bench or playtest evidence.
4. Produce go, conditional-go, or no-go with blockers and evidence.

## Rules
The assessor cannot approve its own missing evidence, waive failing gates, publish, or deploy.

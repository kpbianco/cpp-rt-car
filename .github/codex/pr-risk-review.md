---
on:
  pull_request:
    types: [opened, synchronize, reopened, ready_for_review]

permissions: read-all
engine: codex

safe-outputs:
  add-comment:
---

# System-Risk Pull Request Review

Review the current pull request as a system owner.

Read `AGENTS.md`, `contracts/repo-profile.yaml`, `contracts/verification.yaml`, architecture and decision records, and the full pull-request diff.

Focus on material risks:
- correctness and regression failure modes
- violated architectural or product invariants
- unsafe assumptions and trust-boundary changes
- compatibility, migration, persistence, concurrency, timing, and resource risks
- missing tests for credible failure scenarios
- operational, recovery, deployment, hardware, or playtest gaps required by the repository profile

Do not spend the review on formatting or subjective style unless it creates a concrete defect or maintenance hazard.

Post one concise comment containing:
1. findings ordered by severity, each with file/symbol evidence and a concrete failure scenario
2. validation gaps
3. unanswered questions
4. overall risk: low, medium, high, or blocking

When there are no material findings, say so and list the most important validation that supports that conclusion.

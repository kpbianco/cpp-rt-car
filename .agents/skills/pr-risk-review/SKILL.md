---
name: pr-risk-review
description: Review a pull request for system-level correctness, regressions, violated invariants, security boundaries, missing tests, and operational risk rather than style noise.
---

## Procedure
1. Read the product profile, contracts, relevant architecture, and full diff.
2. Identify affected invariants, consumers, migrations, and operational paths.
3. Check tests against likely failure modes, not only changed lines.
4. Rank findings by severity and confidence.
5. Cite exact files, symbols, and scenarios.
6. Avoid stylistic findings unless they create material maintenance or defect risk.

## Output
Findings first, then unanswered questions, validation gaps, and a concise risk summary. No finding without a concrete failure mode.

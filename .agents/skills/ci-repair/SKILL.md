---
name: ci-repair
description: Apply a bounded repair to a trusted pull-request branch after an evidence-backed CI diagnosis.
---

## Preconditions
- Maintainer-triggered invocation.
- Clean isolated branch or worktree.
- Diagnosis identifies a likely causal failure.
- Allowed paths and validation are explicit.

## Procedure
1. Make the smallest repair.
2. Do not rewrite unrelated code or weaken tests.
3. Run the failing check, then the broader required gate set.
4. Allow at most two repair iterations.
5. Stop on ambiguity, scope expansion, credential need, or protected-path changes.

## Outputs
Patch, validation evidence, and remaining uncertainty. Never merge or deploy.

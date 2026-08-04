---
name: implement-batch
description: Implement an approved batch in an isolated branch or worktree while preserving scope, invariants, and unrelated work.
---

## Rules
- Read the batch contract and repository guidance first.
- Do not broaden scope silently.
- Prefer the smallest coherent implementation.
- Add or update tests with the behavior change.
- Run narrow checks before broad checks.
- Do not commit, push, merge, release, or deploy unless explicitly requested.

## Required output
- Code and tests.
- Updated documentation when interfaces or operation change.
- Evidence draft listing commands, results, residual risks, and unperformed validation.

## Stop conditions
Stop when a product decision is required, a forbidden path must change, validation cannot be made credible, or the batch contract is contradictory.

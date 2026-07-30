---
name: rtfw-assurance
description: Implement and verify an approved RTFW batch while preserving bounded execution, ABI/package compatibility, truthful qualification claims, and retained evidence.
---

# RTFW assurance delivery

## Inputs

- `AGENTS.md`
- `contracts/repo-profile.yaml`
- `contracts/profile-requirements.yaml`
- `contracts/active-batch.yaml`
- `contracts/verification.yaml`
- repository product, architecture, component, ABI, release, and support contracts
- current branch, diff, tests, CI, and recent history

## Procedure

1. Confirm the target repository, branch, baseline, worktree status, and active
   batch. Preserve unrelated work.
2. Read the files named by the batch and inspect the current implementation,
   tests, build graph, package exports, and release-contract checks.
3. Map each acceptance item to code ownership, tests, commands, manual gates,
   and claim limits before editing.
4. Implement only allowed scope. Keep public additions additive unless the batch
   explicitly authorizes versioning. Do not route supported runtime work through
   experimental SimCore, scheduler, fiber, or plugin paths.
5. Check bounded behavior, lifetime/rollback, concurrency, allocation, multiple
   instances, malformed input, compatibility, package isolation, and failure
   recovery relevant to the change.
6. Run focused tests, then the narrowest applicable verification profile, and
   finally `./scripts/agent-verify.sh full`.
7. Create `docs/evidence/<batch>-<date>.md` with:
   - baseline and changed files;
   - acceptance mapping;
   - exact commands, exit codes, and relevant artifacts;
   - changed and preserved invariants;
   - residual risks;
   - validation not performed;
   - explicit RT/hardware/qualification claim boundary.
8. Inspect the full diff for scope, generated artifacts, placeholders, stale
   docs, compatibility drift, and overclaims.

## Stop conditions

Stop and report rather than improvising when:

- an unapproved product or architecture decision is required;
- a forbidden path or protected invariant must change;
- C ABI v8, SONAME 8, device ABI v1, license, or support claims would change
  outside an approved versioning batch;
- hardware, privileged host access, secrets, or controlled runners are required
  for credible evidence;
- the active batch is contradictory, stale, or cannot be verified;
- repair requires weakening tests or deterministic gates.

## Exit criteria

Every acceptance item is pass, fail, blocked, or unverified with evidence. The
diff is scoped, deterministic checks have run, compatibility status is explicit,
and no unperformed hardware or RT qualification is claimed.

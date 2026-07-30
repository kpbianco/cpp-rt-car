# Agentic development runbook

## 1. Establish state

```bash
git status --short
git branch --show-current
git log -1 --oneline
```

Read `AGENTS.md`, the active batch, current state, handoff, relevant component
contracts, tests, build files, workflows, and recent history. Do not overwrite
unrelated work.

## 2. Isolate the batch

Create a branch or worktree from the approved baseline. Use one batch per branch
unless an approved repair is strictly part of that batch.

```bash
git switch -c agent/m15-01-policy-model
```

## 3. Map acceptance before editing

For each acceptance item, identify:

- owning files and symbols;
- focused tests;
- local and CI gates;
- compatibility and release-contract impact;
- manual or hardware validation;
- rollback and stop conditions.

Stop if the batch is stale or a protected decision is required.

## 4. Implement narrowly

Preserve the supported runtime boundary. Add tests with behavior. Do not weaken
checks, hide unsupported behavior behind a successful status, or turn requested
configuration into an applied/qualified claim.

## 5. Verify

Run focused tests while iterating, then:

```bash
./scripts/agent-verify.sh contract
./scripts/agent-verify.sh quick
./scripts/agent-verify.sh full
```

The runner logs under `docs/evidence/local/`. These logs are local diagnostics,
not committed qualification records.

## 6. Record retained evidence

Create `docs/evidence/<batch>-<date>.md` containing:

- baseline, environment, and changed files;
- acceptance criterion -> evidence mapping;
- exact commands and results;
- compatibility, package, allocation, concurrency, and claim review;
- residual risks and rollback;
- validation not performed.

Never label mock, portable, hosted, or preflight evidence as physical hardware,
RT1, or RT2 qualification.

## 7. Publish only when authorized

Inspect the complete diff and status. Stage only intended files. Commit, push,
and open a draft PR only after explicit instruction. Never merge, release,
deploy, change secrets/settings, or waive failing deterministic CI.

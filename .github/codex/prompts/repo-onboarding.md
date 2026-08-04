You are onboarding this repository into a governed multi-repository Codex delivery system.

Profile: {{PROFILE}}
Control plane: {{CONTROL_REPO}}

Read the repository's existing AGENTS.md, README, architecture, handoff, CI workflows, build files, tests, scripts, and recent git history before editing.

Allowed changes are limited to agentic-delivery infrastructure:
- AGENTS.md managed block
- .codex/
- .agents/skills/
- contracts/
- docs/plans/, docs/evidence/, docs/runbooks/
- scripts/agent-*.sh
- .github/codex/

Do not modify product source code. Do not commit or push.

Tasks:
1. Identify the exact deterministic build, test, lint, scan, benchmark, package, and smoke-test commands that already work in this repository.
2. Replace contracts/verification.commands with the narrow-to-broad non-interactive command sequence. Do not invent commands; verify help/configuration or run safe read-only discovery when necessary.
3. Update contracts/verification.yaml with the validation levels this repository actually supports today: static, simulated, protocol, bench, field, playtest, staging, or production.
4. Refine the managed AGENTS.md block with repository-specific mandatory commands, protected paths, invariants, and explicit validation limitations. Keep the entire AGENTS.md concise.
5. Create docs/plans/AGENTIC_DELIVERY_GAPS.md describing missing deterministic CI gates, fixtures, replay harnesses, evidence capture, and release checks. Separate immediate gaps from later improvements.
6. Ensure scripts/agent-verify.sh runs the listed commands fail-fast and records logs under docs/evidence/local/.
7. Inspect the diff and remove generic filler. Leave placeholders only where physical hardware, credentials, live services, or human playtests are genuinely required.

Finish with a concise report of changed files, commands actually run, unresolved gaps, and anything requiring manual validation.

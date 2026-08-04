---
name: verify-change
description: Verify a change against its acceptance criteria using deterministic commands and record evidence without overstating validation.
---

## Procedure
1. Map every acceptance criterion to one or more checks.
2. Run commands from contracts/verification.commands in fail-fast order.
3. Capture command, exit code, relevant output, environment, and artifact paths.
4. Distinguish static, simulated, protocol, bench, field, playtest, staging, and production evidence.
5. Record skipped or impossible checks explicitly.
6. Produce evidence.json conforming to contracts/evidence.schema.json.

## Exit criteria
Every acceptance item is pass, fail, blocked, or unverified with evidence.

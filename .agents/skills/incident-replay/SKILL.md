---
name: incident-replay
description: Convert historical regressions, incidents, escaped defects, or bad pull requests into replayable evaluation fixtures for prompts, skills, and workflows.
---

## Procedure
1. Reconstruct the pre-fix repository state and triggering change.
2. Define the risk that a useful reviewer or CI agent should detect.
3. Run the candidate workflow without revealing the historical answer.
4. Score detection, precision, evidence quality, and recommended action.
5. Store the fixture and scorecard.
6. Improve the skill or workflow only when aggregate evidence supports the change.

## Exit criteria
The evaluation is repeatable and distinguishes useful detection from generic warning language.

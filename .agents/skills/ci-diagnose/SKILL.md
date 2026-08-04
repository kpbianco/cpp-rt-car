---
name: ci-diagnose
description: Diagnose failed CI by finding the first causal failure, classifying it, gathering evidence, and recommending the smallest credible repair without editing code.
---

## Procedure
1. Enumerate failed jobs and steps.
2. Separate causal failures from cascading failures.
3. Reproduce locally when safe and practical.
4. Classify: product defect, test defect, flake, dependency/environment, workflow/config, or unknown.
5. Cite logs, files, symbols, commits, and environmental differences.
6. State confidence, recommended repair, required validation, and whether automated PR repair is safe.

## Exit criteria
The diagnosis conforms to contracts/ci-diagnosis.schema.json and does not claim certainty unsupported by evidence.

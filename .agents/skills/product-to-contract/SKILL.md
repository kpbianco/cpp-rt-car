---
name: product-to-contract
description: Compile approved product inputs into product, architecture, acceptance, roadmap, risk, telemetry, and repository contracts.
---

## Required reads
- products/<slug>/business-plan.md
- profiles/<profile>.yaml
- contracts/product.schema.json

## Procedure
1. Produce product.yaml conforming to the schema.
2. Write PRD.md with capabilities, boundaries, users, and success metrics.
3. Write architecture.md with components, data flows, trust boundaries, and tradeoffs.
4. Write acceptance.yaml with observable capability-level checks.
5. Write roadmap.yaml as dependency-ordered milestones.
6. Write risk-register.yaml, telemetry-plan.yaml, and repos.yaml.
7. Validate internal consistency and list unresolved decisions.

## Exit criteria
No milestone exists without an outcome, acceptance condition, owner repository, risk class, and validation strategy.

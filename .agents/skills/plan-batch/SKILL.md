---
name: plan-batch
description: Turn an approved milestone into a small implementation batch with bounded paths, risks, acceptance criteria, validation, rollback, and evidence requirements.
---

## Inputs
- Product and architecture contracts.
- Current repository state.
- Milestone dependency graph.

## Procedure
1. Map affected components and prerequisites.
2. Choose the smallest coherent vertical change.
3. Define allowed and forbidden paths.
4. State acceptance as observable behavior.
5. Specify exact deterministic validation and manual gates.
6. Identify rollback and residual hazards.
7. Validate against contracts/batch.schema.json.

## Exit criteria
A builder can implement the batch without making new product decisions or touching unrelated systems.

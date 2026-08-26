# Portable assurance

M20-PRE-01 adds deterministic host-independent verification around the
existing portable RT0 product. It changes no production runtime source, public
header, ABI, schema, installed SDK inventory, release version, support matrix,
qualification state, release workflow, or prior evidence.

## Entry point and outputs

Run one mode with an explicit output root and complete expected source commit:

```bash
scripts/verify-portable-assurance.sh all \
  --build-dir build/m20-pre-01 \
  --source-commit "$(git rev-parse HEAD)"
```

The modes are `dependencies`, `static`, `fuzz`, `artifacts`, and cumulative
`all`. A missing tool, version mismatch, skipped input, diagnostic, sanitizer
finding, inventory drift, or verification failure returns nonzero. Generated
builds, corpora, reports, and package candidates stay below the explicit build
directory and are never written to a tag or release.

## Gates

### Dependencies

`tools/sbom.py verify-dependencies` reconciles the initialized and clean
GoogleTest submodule, the GoogleTest fallback URL and SHA-256, RapidCheck's full
commit, the vcpkg baseline, every FetchContent declaration, and every workflow
action against `tools/sbom_expected.json`. Pull-request dependency review is
also blocking at high severity. Pinning proves input identity and immutability;
it is not proof of vulnerability absence, license clearance, or upstream
trustworthiness.

### Static analysis

Clang/clang-tidy 14 analyze the exact sorted `target|source` manifest in
`tools/static_analysis_sources.txt` for a default non-experimental, tests-off,
examples-off, demo-off compilation database. The checker rejects additions,
omissions, duplicates, external/generated redirection, and tool-version drift.
Analyzer core, C++, dead-code, security, Unix, and variadic-list checks are
warnings-as-errors. The padding-optimization diagnostic is excluded because
reordering frozen public and compatibility records is outside this batch;
there is no diagnostic baseline or wildcard source suppression.

### Bounded fuzz smoke

Clang 14 libFuzzer plus AddressSanitizer and UndefinedBehaviorSanitizer replay
every immutable named seed before fixed-seed mutation runs:

| Harness | Classification | Maximum input | Mutation runs |
| --- | --- | ---: | ---: |
| checkpoint/input-log snapshot inspector | supported parser | 64 KiB | 20,000 |
| runtime-profile parser | supported parser | 64 KiB | 20,000 |
| bounded job queue | experimental | 4 KiB | 10,000 |

The runner records tool identity, flags, seed/dictionary hashes, replay and
mutation counts, limits, exit classification, and failure-artifact inventory.
The source corpora remain read-only; generated corpora and crash, timeout, or
out-of-memory artifacts are confined to the build root. This deterministic
smoke is not continuous fuzzing, coverage of every parser, or proof that no
vulnerability exists.

### Candidate package, SBOM, provenance, and manifest

Artifact mode builds the default Release package without experimental code or
vendor devices, stages the CPack archive and checksum, and produces two
independent byte-identical canonical SPDX 2.3 JSON documents. The pinned
official SPDX schema is retained at `release/schemas/spdx-schema-2.3.json`.
The specialized offline verifier enforces the exact M20-PRE-01 SPDX shape,
artifact bytes, RTFW identity, source commit, declared build inputs, and
shipped-versus-build/test relationships; it is not a general-purpose SPDX
conformance service.

`tools/provenance.py` creates a canonical unsigned in-toto Statement v1
candidate. Its subjects bind the archive, checksum, and SBOM; its predicate
binds source commit/tree/cleanliness, tool identities, build parameters, and
declared dependencies. `authentication` is always false. The strict final
manifest covers the archive, checksum, SBOM, and statement and requires the
caller's exact expected source commit before extraction and relocated consumer
tests.

### Public signed fixture

`tests/provenance_fixtures/public/` contains fictional, CC0 non-target fixture
material: one artifact, one DSSE envelope, and public RSA trust data. The
standard-library verifier checks DSSE pre-authentication encoding and RSA
PKCS#1 v1.5 SHA-256 plus exact repository, source, ref, workflow, issuer, and
predicate policy. Mutation tests cover the artifact, payload, signature,
public root, and identity fields. No private key, signing operation, OIDC
request, trust-root refresh, or network lookup exists in the verification
path.

This fixture proves only that the pinned offline verifier accepts and rejects
the retained test material under the retained public trust snapshot. It does
not authenticate the unsigned RTFW candidate, establish current revocation
state, authorize the fictional producer, or claim a SLSA level.

## CI and claim boundary

The `Portable assurance (Clang 14)` CI job runs the cumulative lane on Ubuntu
22.04 with repository permissions limited to `contents: read`. Candidate
reports may be retained as CI evidence. The job has no `id-token`,
attestation, package, or content write permission and performs no signing,
publication, release, deployment, support promotion, device access, privileged
host operation, controlled timing, or Unreal work.

Passing M20-PRE-01 may establish only the recorded deterministic dependency,
analyzer, bounded-fuzz, candidate-content, public-fixture-cryptography, package,
ABI, and portable RT0 results. Continuous fuzzing, authenticated target
provenance, bit-for-bit reproducible or signed releases, controlled
performance, soak/endurance, physical CUDA/XDMA/combined behavior, HIL, field,
RT1/RT2, Unreal integration, deployment, and production readiness remain
separate work.

# Oracle Layer

## 1. Executive Summary

The Oracle Layer is the cold-path subsystem that turns market metadata,
human-approved logical rules, feasible state enumeration, payoff tables, and
candidate bundle definitions into a versioned artifact.

The Oracle Layer does not run in the trading hot path. It exists to prepare a
deterministic, auditable artifact that runtime systems can load without calling
external APIs, LLM providers, or solvers.

The core invariant is:

```text
same market snapshot + same approved rulebook + same bundle fixture
  -> same constraints
  -> same feasible states
  -> same payoff matrix
  -> same candidate bundle artifact
  -> same checksums
```

## 2. Scope / Non-Scope

In scope:

- Load market metadata from fixture snapshots.
- Preserve Polymarket asset IDs as strings.
- Load and validate manual rulebooks.
- Reject unapproved rules before compilation.
- Compile approved rules into Boolean linear constraints.
- Enumerate small feasible state spaces.
- Build binary-outcome payoff matrices.
- Validate candidate bundle structure.
- Export versioned Oracle artifacts.
- Load artifacts back and verify checksums.
- Provide a workflow-level verification tool.

Non-scope:

- No runtime trading signals.
- No order execution.
- No risk decisions.
- No live API dependency in runtime.
- No live LLM dependency in runtime.
- No large-scale solver in v0.
- No automatic trusted rule approval from LLM output.
- No hot-path market state mutation.

## 3. Cold Path Design

The Oracle Layer is a build-time or research-time pipeline:

```text
RawMarketRecord fixture
  -> MarketUniverseBuilder
  -> Manual Rulebook
  -> RuleValidator
  -> ConstraintCompiler
  -> StateEnumerator
  -> PayoffMatrixBuilder
  -> CandidateBundleGenerator / BundleValidator
  -> ArtifactExporter
  -> ArtifactLoader verification
```

The output is an artifact directory under:

```text
artifacts/oracle/<artifact_id>/
```

The runtime boundary is intentionally narrow:

```text
runtime loads artifact
runtime does not call Market API
runtime does not call LLM
runtime does not run solver
```

## 4. Inputs

### Market API

The live metadata source is Polymarket Gamma Markets API:

```text
https://gamma-api.polymarket.com/markets
```

`MarketApiClient` maps Gamma market records into `RawMarketRecord`:

- `conditionId` -> `market_id`
- first event `id` -> `event_id`
- `question` / `description` / event title / event description -> rule text
- `outcomes` -> outcome names
- `clobTokenIds` -> Polymarket CLOB asset IDs
- `resolutionSource` / `endDate` -> resolution metadata

The fetch tool can produce a real snapshot:

```text
build/fetch_market_universe \
  --polymarket-live \
  --limit 10 \
  --out runs/oracle_live_polymarket/raw_markets.jsonl \
  --candidate-bundles-out runs/oracle_live_polymarket/candidate_bundles.json
```

Live API access is not required by default tests. Tests use synthetic Gamma
responses to keep CI deterministic and network-free.

### Fixture

The current deterministic input is:

```text
tests/fixtures/oracle/raw_markets_small.jsonl
```

Each `RawMarketRecord` contains:

- `market_id`
- `event_id`
- `title`
- `description`
- `outcomes`
- `asset_ids`
- `resolution_source`
- `end_time`
- `tags`
- `fetched_at_ns`
- `source`

The `outcomes` and `asset_ids` arrays must have matching counts. Asset IDs are
preserved as strings because Polymarket token IDs can exceed normal integer
comfort ranges for application logic.

### Manual Rulebook

The current fixture rulebook is:

```text
tests/fixtures/oracle/rulebook_small.json
```

Only approved `ValidatedRule` records are compiler eligible.

### Future LLM RuleDraft

LLM extraction is allowed only to produce `RuleDraft` records. It must not
produce compiler-ready rules. `RuleDraft` defaults to:

```text
requires_manual_review = true
```

Manual review must turn a draft into an approved `ValidatedRule` before the
compiler can use it.

## 5. Rule Lifecycle

Rule lifecycle:

```text
market text / fixture / optional LLM extraction
  -> RuleDraft
  -> manual review
  -> ValidatedRule
  -> Rulebook
  -> RuleValidator
  -> ConstraintCompiler
```

`RuleDraft` is not trusted. It is an authoring artifact.

`ValidatedRule` is trusted only when:

```text
approved = true
approved_by is present
approved_at_ns is present
```

Unapproved rules must not enter the compiler. If a rulebook contains
unapproved rules, `verify_oracle_workflow` fails the compiler-ready check.

## 6. Constraint Compilation

The v0 compiler supports:

```text
ExactlyOne(A, B)      -> A + B = 1
AtMostOne(A, B, C)   -> A + B + C <= 1
AtLeastOne(A, B, C)  -> A + B + C >= 1
Implies(A -> B)      -> A - B <= 0
```

Compiler output:

```text
BooleanVariable[]
LinearBooleanConstraint[]
constraint_hash
```

Variable ordering is deterministic. Constraint hashing is deterministic.

Unsupported rule types are rejected in v0 rather than silently ignored.

## 7. State Enumeration

The v0 enumerator is brute force and intentionally limited:

```text
variable_count <= 32
```

For every bitset, the enumerator evaluates every linear Boolean constraint:

```text
sum(coeff_i * x_i) op rhs
```

If all constraints pass, the state is feasible.

Contradictory rulebooks produce:

```text
feasible_states = 0
contradictions = 1
workflow failure
```

This is sufficient for the current small fixture workflow. Larger market
universes need a later solver phase.

The workflow no longer treats global enumeration as the only valid path. After
constraint compilation, Oracle also builds a constraint graph and partitions the
rulebook into connected components:

```text
CompiledConstraintSet
  -> ConstraintGraphBuilder
  -> ComponentPartitioner
  -> CompiledComponent[]
```

Variables are graph nodes. Constraints are hyperedges. Variables joined by the
same constraint are placed in the same component. This is deliberately based on
the constraint graph rather than only `event_id`, because rules can connect
markets across events and a single event can contain unrelated subgraphs.

Large components are accepted only when they have an explicit oracle backend.
Current semantic backends:

```text
ExactlyOneOracle
AtMostOneOracle
```

Small components can still use `StateEnumerator`. If the full variable set is
larger than 32 variables, `verify_oracle_workflow` switches to:

```text
enumeration_mode: component_oracle
```

In this mode the artifact records components and oracle descriptors, while
`feasible_states.bin` and `payoff_matrix.bin` may be empty placeholders. This
keeps large components from forcing a global `2^N` state explosion.

Example: the 2026 FIFA World Cup winner event has 96 variables in the local
market snapshot and one LLM-approved `AtMostOne` constraint over 39 YES
variables. The workflow now emits:

```text
component_count: 58
max_component_variable_count: 39
at_most_one_components: 1
skipped_full_enumeration_count: 1
large_component_without_oracle_count: 0
enumeration_mode: component_oracle
manifest_ok: true
checksums_ok: true
determinism_passed: true
```

This proves the large logical component can be represented and verified without
materializing the full payoff matrix. Runtime pricing and Top-N leg selection
remain Signal Engine responsibilities.

## 8. Payoff Matrix

The v0 payoff model is binary:

```text
variable true  -> payout_tick = PAYOUT_ONE_TICK
variable false -> payout_tick = 0
```

Current scale:

```text
PAYOUT_ONE_TICK = 1000000
```

Output shape:

```text
feasible_state_count x asset_count
```

Every feasible state and every asset must have a payoff entry. Negative payout
entries are invalid.

## 9. Candidate Bundle Format

Candidate bundle authoring remains string-based:

```text
BundleLeg:
  market_id
  asset_id
  side
  quantity_lots
  max_price_tick

CandidateBundle:
  bundle_id
  required_true_mask
  required_false_mask
  invalid_mask
  guaranteed_payout_tick
  leg_count
  legs[16]
  min_edge_tick
```

Structural validation checks:

- `leg_count > 0`
- `leg_count <= 16`
- `asset_id` exists
- `market_id` exists
- `quantity_lots > 0`
- `max_price_tick >= 0`
- required masks do not conflict
- `bundle_id` is unique

There are two bundle-generation paths:

```text
CandidateBundleGenerator
  loads fixture bundles
  validates structure
  keeps legacy metadata-stage buy-all-outcomes support

CombinatorialBundleGenerator
  consumes approved Rulebook + market universe
  generates cross-market bundles from compiled logic
  skips trivial single-market ExactlyOne(Yes, No) rules
```

Rulebook-driven bundle generation is the path for combinatorial arbitrage. It
uses only approved rules:

```text
ExactlyOne(YES_1..YES_N)
  -> Buy YES_1..YES_N
  -> guaranteed_payout_tick = PAYOUT_ONE_TICK

ExactlyOne(YES_1..YES_N)
  -> Buy NO_1..NO_N
  -> guaranteed_payout_tick = (N - 1) * PAYOUT_ONE_TICK

AtMostOne(YES_1..YES_N)
  -> Buy NO_1..NO_N
  -> guaranteed_payout_tick = (N - 1) * PAYOUT_ONE_TICK
```

The generator deliberately does not emit `AtMostOne` YES baskets. `AtMostOne`
does not prove that one listed outcome must occur, so buying all listed YES legs
can lose if an outside outcome wins. This is the main distinction between
complete bracket rules and partial winner-set rules.

The generator also refuses rules that exceed `kMaxBundleLegs = 16`. Wider
winner sets need a later pruning/ranking phase before they can become hot-path
candidate bundles.

Workflow verification can generate bundles directly from the approved Rulebook:

```text
build/verify_oracle_workflow \
  --market-snapshot runs/oracle_live_polymarket_2000_paginated/raw_markets.jsonl \
  --rulebook runs/oracle_live_polymarket_2000_paginated/rulebook_24383_approved.json \
  --generate-combinatorial-bundles \
  --out runs/oracle_live_polymarket_2000_paginated/artifact_24383 \
  --check-determinism
```

Neither generator reads live prices or emits live opportunities. Signal Engine
is still responsible for depth, cost, and edge evaluation.

## 10. Artifact Layout

Artifact output:

```text
artifacts/oracle/<artifact_id>/
├── manifest.json
├── market_universe.json
├── rulebook.json
├── variables.bin
├── constraints.bin
├── feasible_states.bin
├── payoff_matrix.bin
├── candidate_bundles.bin
├── market_dependency_graph.bin
├── settlement_bitmask.bin
└── checksums.txt
```

`checksums.txt` records deterministic FNV-1a 64-bit hashes:

```text
manifest.json <hash>
market_universe.json <hash>
rulebook.json <hash>
variables.bin <hash>
constraints.bin <hash>
feasible_states.bin <hash>
payoff_matrix.bin <hash>
candidate_bundles.bin <hash>
```

The artifact manifest records:

- `llm_enabled`
- `llm_provider`
- `llm_outputs_used`
- `llm_outputs_require_manual_review`
- `input_snapshot_hash`
- `rulebook_hash`
- `constraint_hash`
- `feasible_states_hash`
- `payoff_hash`
- `bundle_hash`

`ArtifactLoader` must reject checksum mismatches.

## 11. LLM Integration Policy

Default policy:

```text
ORACLE_ENABLE_LLM = OFF
```

Build and tests must pass when:

```text
OPENROUTER_API_KEY is unset
```

LLM output policy:

- LLM output can only become `RuleDraft`.
- LLM output cannot become `ValidatedRule` directly.
- LLM output cannot enter `ConstraintCompiler` directly.
- `RuleDraft` defaults to `requires_manual_review = true`.
- Manual approval is required before compilation.

Default extractors:

```text
StubRuleExtractor:
  no network
  returns Disabled
  produces no ValidatedRule

OpenRouterRuleExtractor:
  disabled when ORACLE_ENABLE_LLM=OFF
  targets meta-llama/llama-3.3-70b-instruct:free
  uses https://openrouter.ai/api/v1/chat/completions
  returns MissingApiKey when enabled without OPENROUTER_API_KEY
  allows model override through OPENROUTER_MODEL or --model
  allows output-token cap through OPENROUTER_MAX_TOKENS or --max-tokens
  groups market context by event_id
  instructs the model to emit only cross-market combinatorial constraints
  rejects trivial single-market YES/NO constraints in the prompt
  sends reasoning.enabled=false and response_format=json_object for compact drafts
  reports non-2xx HTTP status, Retry-After, and response body diagnostics
  not used by default tests
```

Manual approval flow:

```text
OpenRouterRuleExtractor
  -> RuleDraft[]
  -> ManualRuleEditor review / approve
  -> approved Rulebook
  -> RuleValidator
  -> ConstraintCompiler
```

LLM extraction command:

```text
build/extract_oracle_rules \
  --use-llm \
  --market-snapshot tests/fixtures/oracle/raw_markets_small.jsonl \
  --drafts-out runs/oracle_rule_drafts.json
```

For real Polymarket event groups, narrow extraction to one event at a time:

```text
build/extract_oracle_rules \
  --use-llm \
  --model nvidia/llama-3.3-nemotron-super-49b-v1.5 \
  --max-tokens 10000 \
  --event-id 24383 \
  --market-snapshot runs/oracle_live_polymarket_2000_paginated/raw_markets.jsonl \
  --drafts-out runs/oracle_live_polymarket_2000_paginated/rule_drafts_24383.json
```

This is intended for combinatorial rules such as sentencing brackets,
tournament winners, and election nominee sets. The LLM output is still only
`RuleDraft`; it must be manually reviewed and approved before compilation.

Minimal paid OpenRouter smoke with NVIDIA Nemotron:

```text
build/extract_oracle_rules \
  --use-llm \
  --model nvidia/llama-3.3-nemotron-super-49b-v1.5 \
  --max-tokens 256 \
  --market-snapshot tests/fixtures/oracle/raw_markets_small.jsonl \
  --drafts-out runs/oracle_rule_drafts.json
```

If the default free model returns `429`, either wait for the provider limit to
reset or run with a different OpenRouter model:

```text
OPENROUTER_MODEL="<provider/model>" \
build/extract_oracle_rules \
  --use-llm \
  --market-snapshot tests/fixtures/oracle/raw_markets_small.jsonl \
  --drafts-out runs/oracle_rule_drafts.json

build/extract_oracle_rules \
  --use-llm \
  --model "<provider/model>" \
  --market-snapshot tests/fixtures/oracle/raw_markets_small.jsonl \
  --drafts-out runs/oracle_rule_drafts.json
```

Manual approval command:

```text
build/extract_oracle_rules \
  --drafts-in runs/oracle_rule_drafts.json \
  --approve-drafts \
  --approved-by manual \
  --approved-at-ns 1 \
  --approved-rulebook-out runs/oracle_rulebook_approved.json
```

For safety, approval is explicit. LLM drafts are never compiler-ready until a
human or an intentionally configured manual workflow writes an approved
rulebook.

## 12. CMake / Environment

CMake option:

```cmake
option(ORACLE_ENABLE_LLM "Enable live LLM rule extraction" OFF)
```

Core targets:

```text
oracle_core
oracle_ingestion
oracle_rules
oracle_llm
oracle_compiler
oracle_enumerate
oracle_payoff
oracle_bundles
oracle_artifact
oracle_tools
```

Default build must not require:

- `OPENROUTER_API_KEY`
- live network
- Market API availability
- LLM provider availability

## 13. Workflow Verification

Workflow verification tool:

```text
build/verify_oracle_workflow \
  --market-snapshot tests/fixtures/oracle/raw_markets_small.jsonl \
  --rulebook tests/fixtures/oracle/rulebook_small.json \
  --candidate-bundles tests/fixtures/oracle/candidate_bundles_small.json \
  --out /tmp/oracle_artifact \
  --check-determinism
```

Expected summary shape:

```text
ingestion:
  markets_loaded:
  assets_loaded:
  missing_fields:

rules:
  approved_rules:
  unapproved_rules:
  validation_errors:

constraints:
  variables:
  constraints:
  contradictions:

states:
  feasible_states:
  enumeration_mode:

payoff:
  rows:
  columns:
  invalid_entries:

bundles:
  candidate_bundles:
  rejected_bundles:

artifacts:
  manifest_ok:
  checksums_ok:
  determinism_passed:

llm:
  enabled:
  provider:
  outputs_used:
```

Small fixture acceptance:

```text
markets_loaded > 0
assets_loaded > 0
approved_rules > 0
unapproved_rules = 0
validation_errors = 0
variables > 0
constraints > 0
contradictions = 0
feasible_states > 0
payoff rows > 0
manifest_ok = true
checksums_ok = true
determinism_passed = true
llm.outputs_used = false
```

## 14. Failure Conditions

Workflow failure conditions include:

- market fixture cannot be loaded
- missing `market_id`
- outcome count differs from asset ID count
- unknown variable in rulebook
- duplicate rule ID
- unapproved rule present in compiler path
- unsupported compiler rule type
- more than 32 variables in brute force enumeration
- contradictory constraints with zero feasible states
- payoff matrix contains invalid entries
- candidate bundle references unknown market or asset
- candidate bundle has conflicting masks
- artifact write failure
- artifact read failure
- checksum mismatch
- LLM output used without manual approval

## 15. Runtime Boundary

Runtime is artifact-only:

```text
Oracle cold path:
  API / fixture / rulebook / LLM draft / compiler / solver / artifact exporter

Trading runtime:
  ArtifactLoader
  read-only artifact data
```

Runtime must not:

- call Market API
- call LLM providers
- run `ConstraintCompiler`
- run `StateEnumerator`
- run a solver
- modify Oracle artifacts
- approve rules
- infer rules from text

Runtime can:

- load artifact files
- validate checksums
- use precomputed constraints, payoff, and candidate bundles
- reject startup if artifact validation fails

## 16. Current Limitations

Current v0 limitations:

- Fixture ingestion is the primary deterministic metadata path.
- Polymarket Gamma ingestion is available for cold-path snapshot generation,
  but default CI does not depend on network access.
- LLM integration is a disabled placeholder by default.
- Only small brute force enumeration is supported.
- `variable_count > 32` is rejected.
- Payoff model is binary only.
- Candidate bundle generation supports fixture bundles and conservative
  buy-all-outcomes structural bundles. Markets with split / proportional /
  50-50 resolution text are skipped.
- No live price integration.
- No runtime opportunity generation.
- No production solver.
- No artifact schema migration mechanism beyond `artifact_version`.

## 17. Next Steps

Recommended next work:

1. Add artifact schema version validation at runtime startup.
2. Add a read-only runtime artifact facade for strategy-facing code.
3. Add larger-market solver design without changing v0 brute force behavior.
4. Add richer market dependency graph generation.
5. Add settlement bitmask compilation once settlement semantics are explicit.
6. Add offline API snapshot ingestion with deterministic persisted outputs.
7. Add manual rule review tooling before enabling live LLM extraction.

## Recommended Commit Split

Suggested commits:

```text
Commit 1: Step 12.1 oracle skeleton + manifest + CMake
Commit 2: Step 12.2 RawMarketRecord + fixture ingestion
Commit 3: Step 12.3 Rulebook / ManualRuleEditor
Commit 4: Step 12.4 LLM interface + stub + OpenRouter placeholder
Commit 5: Step 12.5 ConstraintCompiler
Commit 6: Step 12.6 FeasibilityChecker + StateEnumerator
Commit 7: Step 12.7 PayoffMatrixBuilder
Commit 8: Step 12.8 CandidateBundle model + validator
Commit 9: Step 12.9 ArtifactExporter / Loader / Checksums
Commit 10: Step 12.10 verify_oracle_workflow
Commit 11: Step 12.11 docs/oracle_layer.md
```

## Oracle Layer v0 Acceptance

Oracle Layer v0 is accepted when:

1. `ORACLE_ENABLE_LLM=OFF` build passes.
2. Tests pass without `OPENROUTER_API_KEY`.
3. `StubRuleExtractor` does not access the network.
4. `OpenRouterRuleExtractor` is disabled by default.
5. Market fixtures load into `RawMarketRecord`.
6. Rulebook validation allows only approved rules into the compiler.
7. `ConstraintCompiler` outputs deterministic constraints.
8. `FeasibilityChecker` can detect contradictions.
9. `StateEnumerator` can enumerate small feasible state spaces.
10. `PayoffMatrixBuilder` outputs binary outcome payoff matrices.
11. `CandidateBundleValidator` rejects invalid bundles.
12. `ArtifactExporter` writes complete artifacts.
13. `ArtifactLoader` round-trips exported artifacts.
14. `checksums_ok = true`.
15. `verify_oracle_workflow determinism_passed = true`.
16. Runtime does not depend on API, LLM, or solver execution.

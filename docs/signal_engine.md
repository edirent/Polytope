# Signal Engine

## 1. Executive Summary

The Signal Engine is the paper-opportunity scanner for the trading engine. It
loads candidate bundles from an Oracle artifact, reads market depth from
`MarketStateSnapshot`, checks settlement and market quality, estimates bundle
cost from order book depth, computes theoretical edge, ranks results, and
publishes `OpportunityIntent` records.

The Signal Engine does not execute orders.

The central boundary is:

```text
MarketStateSnapshot + OracleArtifact
  -> SignalEngine
  -> OpportunityIntent
```

`OpportunityIntent` is not an order. It is a structured intent that a later Risk
Engine must approve before any Execution layer can act.

Signal Engine consumes only `MarketStateSnapshot` and `OracleArtifact` inputs.
Signal Engine does not call API, LLM, or solver components.

## 2. Scope / Non-Scope

In scope:

- Load candidate bundles from a verified Oracle artifact.
- Read `MarketStateSnapshot` records from fixtures or `MarketStateView`.
- Check settlement masks before pricing.
- Enforce market-state quality gates.
- Estimate bundle cost from depth with `VWAPPrecheck`.
- Subtract cost, fee, and latency buffer from guaranteed payout.
- Rank published intents deterministically.
- Publish paper opportunity and rejection intents.
- Provide workflow-level verification through `verify_signal_workflow`.

Non-scope:

- No order execution.
- No risk approval.
- No position sizing.
- No portfolio exposure management.
- No REST order placement.
- No WebSocket ingestion.
- No raw log reading.
- No JSON decode or normalization.
- No ChainConfirm internals.
- No Market API calls.
- No LLM calls.
- No solver calls.

The Signal Engine consumes already-built runtime state and already-built Oracle
artifacts. It does not construct either one.

This is the no-execution boundary: Signal can describe a paper opportunity, but
it cannot create, approve, or submit an order.

## 3. Inputs

### MarketStateSnapshot

`MarketStateSnapshot` is the only market-state input.

The Signal Engine needs snapshot fields for:

- asset and market identity
- book depth
- best bid / best ask metadata
- book quality flags
- usable-for-depth / usable-for-signal flags
- state hash / snapshot version identity

The Signal Engine must not read `EntityStateStore`, raw packets, raw JSON, or
chain logs directly.

### OracleArtifact

The Signal Engine loads candidate bundles through `OracleArtifactReader`.

Required artifact properties:

- manifest exists
- artifact version is supported
- candidate bundle file exists
- bundle count matches manifest
- checksums pass
- bundle IDs are unique
- bundle leg count is within the configured limit

The Signal Engine does not call the Oracle compiler, rule validator, state
enumerator, payoff builder, API client, LLM extractor, or solver.

## 4. Outputs

The only runtime output is:

```text
OpportunityIntent
```

An intent contains:

- `intent_id`
- `bundle_id`
- status / rejection reason
- settlement validity
- quality-gate result
- depth sufficiency
- guaranteed payout
- estimated cost
- estimated fee
- latency buffer
- estimated edge
- min edge threshold
- priced legs
- snapshot version hash
- Oracle artifact version

`OpportunityIntent` is not an executable order. It has no authority to submit,
cancel, replace, or route orders.

## 5. Module Structure

Current module layout:

```text
engine/signal/
├── public/
│   ├── SignalTypes.h
│   ├── SignalConfig.h
│   ├── OpportunityIntent.h
│   └── SignalResult.h
├── reader/
│   ├── OracleArtifactReader
│   ├── FixtureMarketSnapshotReader
│   └── MarketStateViewSnapshotReader
├── pricing/
│   ├── VWAPPrecheck
│   ├── CostResult
│   ├── PriceVectorBuilder
│   └── FeeModel
├── edge/
│   ├── LatencyBufferModel
│   ├── EdgeBreakdown
│   └── TheoreticalEdgeCalculator
├── rank/
│   └── OpportunityRanker
├── publish/
│   ├── IIntentPublisher
│   ├── CapturingIntentPublisher
│   ├── JsonlIntentWriter
│   └── PaperIntentPublisher
├── core/
│   ├── SignalEngine
│   └── SignalWorkflow
└── tools/
    └── verify_signal_workflow
```

Dependency direction:

```text
engine_signal
  -> state snapshot/view types
  -> oracle public/artifact types
```

Forbidden dependencies:

```text
engine_signal -> engine_feed
engine_signal -> engine_decode internals
engine_signal -> engine_chain_confirm internals
engine_signal -> engine_execution
engine_signal -> engine_risk
```

## 6. CandidateBundle Scanning

`SignalEngine::scan_once()` iterates over active candidate bundles loaded from
the Oracle artifact.

For each bundle:

```text
settlement check
  -> snapshot read / quality gate
  -> VWAP precheck
  -> edge calculation
  -> intent publication candidate
```

`SignalConfig::max_intents_per_scan` caps published intents. Rejections are
published when:

```text
emit_rejections = true
```

Current default:

```text
emit_rejections = true
```

This is intentional for paper validation and debugging.

## 7. Settlement Mask Check

Settlement filtering uses the current settlement masks from
`SignalScanContext`:

```text
current_true_mask
current_false_mask
settlement_masks_available
```

If a candidate bundle is invalid under settlement state, the intent is rejected:

```text
RejectedInvalidSettlement
```

This is a pre-pricing rejection. Invalid settlement must not proceed into depth
or edge calculation.

## 8. MarketState Quality Gate

The snapshot reader gathers every leg's `MarketStateSnapshot` and validates the
state before pricing.

Failure mapping:

```text
missing snapshot                 -> RejectedMissingSnapshot
not usable_for_depth             -> RejectedBadMarketState
recovering / stale / crossed     -> RejectedBadMarketState
closed / resolved                -> RejectedBadMarketState
usable_for_signal required false -> RejectedBadMarketState
```

Current default:

```text
require_usable_for_depth = true
require_usable_for_signal = false
```

This keeps depth usability separate from signal-quality strictness. A future
configuration can require `usable_for_signal` for stricter paper scans.

## 9. VWAPPrecheck

`VWAPPrecheck` estimates cost from book depth. It does not use only best price.

For BUY legs, it consumes ask levels from lowest to highest:

```text
remaining_quantity
  -> take available asks in price order
  -> sum price_tick * quantity_lots
  -> compute leg VWAP and worst price
```

Depth failures:

```text
MissingSnapshot     -> RejectedMissingSnapshot
MissingBookSide     -> RejectedInsufficientDepth
InsufficientDepth   -> RejectedInsufficientDepth
InvalidQuantity     -> RejectedInsufficientDepth
InvalidLeg          -> RejectedInsufficientDepth
```

v0 supports BUY-leg pricing. SELL legs are structurally represented but rejected
by `VWAPPrecheck` until cost/proceeds conventions are explicitly approved.

## 10. Edge Calculation

`TheoreticalEdgeCalculator` computes paper edge:

```text
estimated_edge_tick =
    guaranteed_payout_tick
    - total_cost_tick
    - fee_tick
    - latency_buffer_tick
```

The v0 `FeeModel` returns the configured default fee.

The v0 `LatencyBufferModel` returns the configured default latency buffer.

Threshold:

```text
estimated_edge_tick >= bundle.min_edge_tick
```

If the threshold passes:

```text
PaperOpportunity
```

Otherwise:

```text
RejectedLowEdge
```

`PaperOpportunity` still is not an order.

## 11. Ranking

`OpportunityRanker` sorts deterministically:

```text
1. PaperOpportunity before rejection
2. estimated_edge_tick descending
3. bundle_id ascending
4. intent_id ascending
```

This keeps output ordering stable across repeated scans with identical inputs.

## 12. Publishing

Publishing is synchronous in v0.

Available publishers:

- `CapturingIntentPublisher` for tests
- `JsonlIntentWriter` for tool output
- `PaperIntentPublisher` for paper-path integration

Publishing does not call Execution. It writes or captures `OpportunityIntent`
records only.

## 13. Workflow Verification

Workflow command:

```text
build/verify_signal_workflow \
  --state-snapshot tests/fixtures/signal/market_state_snapshots_small.json \
  --oracle-artifact tests/fixtures/oracle/artifact_small \
  --check-determinism
```

Optional output:

```text
build/verify_signal_workflow \
  --state-snapshot tests/fixtures/signal/market_state_snapshots_small.json \
  --oracle-artifact tests/fixtures/oracle/artifact_small \
  --emit-rejections true \
  --out runs/signal_intents.jsonl \
  --check-determinism
```

Summary shape:

```text
signal_workflow:
  snapshots_loaded:
  candidate_bundles_loaded:
  candidate_bundles_scanned:

quality_gate:
  rejected_missing_snapshot:
  rejected_bad_state:

settlement:
  settlement_masks_available:
  rejected_invalid_settlement:

vwap:
  vwap_checked:
  enough_depth:
  insufficient_depth:

edge:
  edge_computed:
  above_threshold:
  below_threshold:

intents:
  paper_opportunities:
  rejected_invalid_settlement:
  rejected_bad_market_state:
  rejected_insufficient_depth:
  rejected_low_edge:
  intents_published:

hashes:
  signal_output_hash:
  determinism_passed:
```

Current small fixture baseline:

```text
snapshots_loaded: 2
candidate_bundles_loaded: 1
candidate_bundles_scanned: 1
paper_opportunities: 0
rejected_insufficient_depth: 1
intents_published: 1
signal_output_hash: 17898399197160830659
determinism_passed: true
```

`paper_opportunities = 0` is not a workflow failure. Rejection correctness and
deterministic output are sufficient for the small fixture.

Positive fixture command:

```text
build/verify_signal_workflow \
  --state-snapshot tests/fixtures/signal/market_state_snapshots_positive.json \
  --oracle-artifact tests/fixtures/oracle/artifact_positive \
  --emit-rejections true \
  --out runs/signal_positive_intents.jsonl \
  --check-determinism
```

Positive fixture baseline:

```text
candidate_bundles_loaded: 1
candidate_bundles_scanned: 1
paper_opportunities: 1
rejected_insufficient_depth: 0
intents_published: 1
determinism_passed: true
```

The emitted JSONL intent must contain:

```text
"status":"PaperOpportunity"
"enough_depth":true
"estimated_edge_tick":200000
```

## 14. Failure Conditions

The workflow fails if:

```text
candidate_bundles_loaded == 0
candidate_bundles_scanned == 0
determinism_passed == false
emit_rejections == true and intents_published == 0
artifact manifest missing
artifact checksum mismatch
unsupported artifact version
duplicate bundle_id
missing required snapshot
bad state accepted as priceable
insufficient depth accepted as enough depth
below-threshold edge emitted as PaperOpportunity
OpportunityIntent treated as an order
Signal calls API / LLM / solver
Signal depends on Feed / Decode internals / ChainConfirm internals
Signal depends on Risk / Execution
```

Rejection statuses:

```text
RejectedInvalidSettlement
RejectedBadMarketState
RejectedMissingSnapshot
RejectedInsufficientDepth
RejectedLowEdge
```

Successful paper status:

```text
PaperOpportunity
```

Initial status before processing:

```text
CandidateOnly
```

## 15. Runtime Boundary

Runtime boundary:

```text
Market State Layer
  -> MarketStateSnapshot
  -> Signal Engine

Oracle Layer
  -> versioned artifact
  -> Signal Engine

Signal Engine
  -> OpportunityIntent
  -> Risk Engine
  -> Execution Engine
```

The Signal Engine does not:

- mutate market state
- read raw logs
- decode venue payloads
- classify chain fills
- approve risk
- place orders
- call external APIs
- call LLM providers
- run solvers

Risk Engine must approve any intent before execution. Execution must never
treat a raw `OpportunityIntent` as an order.

## 16. Known Limitations

Current v0 limitations:

- BUY-leg VWAP is supported; SELL-leg pricing is rejected until semantics are
  explicitly settled.
- Fee model is fixed-config, not venue-specific.
- Latency buffer model is fixed-config, not dynamic.
- Settlement mask support is minimal.
- Fixture workflow currently proves rejection determinism, not positive live
  profitability.
- No live market scan loop is wired to execution.
- No position, inventory, or exposure constraints are evaluated here.
- No API / LLM / solver access is available in runtime.

These are deliberate boundaries for the current Signal v0.

## 17. Next Layer: Risk Engine

The next layer is Risk Engine.

Risk Engine should consume `OpportunityIntent` and decide whether a paper
opportunity is allowed to become an execution request. It should own:

- exposure limits
- position limits
- capital allocation
- order size limits
- venue risk controls
- kill switches
- final approval / rejection

Only after Risk approval should Execution receive an order-like object.

Signal Engine remains a scanner and intent publisher, not an order execution
system.

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

Signal v0.5 does not support SELL leg pricing unless explicitly enabled. BUY
legs are priced against asks. SELL legs are represented in the type system, but
the default runtime rejects them before edge calculation.

Signal Engine consumes only `MarketStateSnapshot` and `OracleArtifact` inputs.
Signal Engine does not call API, LLM, or solver components.

## 2. Scope / Non-Scope

In scope:

- Load candidate bundles from a verified Oracle artifact.
- Index active bundles through `BundleRegistry`.
- Read `MarketStateSnapshot` records from fixtures or `MarketStateView`.
- Check settlement masks before pricing.
- Enforce market-state quality gates.
- Enforce snapshot consistency across multi-leg bundles.
- Estimate bundle cost from depth with `VWAPPrecheck`.
- Compute executable bundle quantity from per-leg available depth.
- Subtract cost, fee, latency buffer, and slippage buffer from guaranteed payout.
- Compute unit edge, total edge, and edge bps.
- Rank published intents deterministically.
- Deduplicate paper opportunities by idempotency key.
- Rate-limit published paper opportunities.
- Emit signal counters and scan latency placeholders.
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
- artifact / constraint / bundle hashes
- snapshot version and snapshot hash
- executable bundle quantity
- unit edge, total edge, and edge bps
- created and expiry timestamps
- idempotency key
- proof reference
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
│   ├── SideResolver
│   └── FeeModel
├── edge/
│   ├── LatencyBufferModel
│   ├── SlippageBufferModel
│   ├── EdgeBreakdown
│   └── TheoreticalEdgeCalculator
├── rank/
│   └── OpportunityRanker
├── publish/
│   ├── IIntentPublisher
│   ├── IntentBuilder
│   ├── IntentDeduper
│   ├── IntentRateLimiter
│   ├── CapturingIntentPublisher
│   ├── JsonlIntentWriter
│   └── PaperIntentPublisher
├── scan/
│   ├── BundleRegistry
│   ├── CandidateBundleScanner
│   └── DirtyAssetSet
├── metrics/
│   └── SignalMetrics
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
the Oracle artifact. v0.5 introduces `BundleRegistry` so the scanner can use a
stable active-bundle view plus an asset-to-bundle inverted index.

Supported scan modes:

```text
full scan
dirty asset scan
```

The workflow tool still uses full scan by default. Dirty-asset scan is the
runtime shape for future event-driven scans where only bundles affected by
changed assets need to be repriced.

For each bundle:

```text
settlement check
  -> snapshot read / consistency guard / quality gate
  -> price vector
  -> VWAP precheck
  -> edge calculation
  -> intent build
  -> rank
  -> dedupe
  -> rate limit
  -> publish
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

## 8. Snapshot Consistency

Multi-leg bundles must not be priced from mutually inconsistent book versions.
For example, leg A from book version 100 and leg B from book version 127 can
manufacture a false edge that never existed as one market state.

The snapshot reader gathers unique asset snapshots for a bundle and runs
`SnapshotConsistencyGuard` before pricing.

`SignalConfig` controls:

```text
max_lob_age_ns
max_snapshot_version_skew
consistency_mode
```

Supported modes:

```text
StrictSameVersion
BoundedSkew
```

Failure mapping:

```text
missing snapshot       -> RejectedMissingSnapshot
stale LOB              -> RejectedBadMarketState
version skew too large -> RejectedBadMarketState
crossed / recovering   -> RejectedBadMarketState
closed / resolved      -> RejectedBadMarketState
```

The workflow reports these separately:

```text
snapshot:
  consistency_checked:
  rejected_snapshot_skew:
  rejected_stale_snapshot:
```

## 9. MarketState Quality Gate

The snapshot reader validates each leg's `MarketStateSnapshot` before pricing.

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

## 10. Executable Bundle Quantity

Signal v0.5 prices a bundle only if every leg has enough executable depth.
The executable bundle quantity is not taken from one leg; it is the limiting
quantity across all legs.

For each BUY leg:

```text
target quantity
  -> consume asks from lowest to highest
  -> compute executable leg quantity
  -> compute leg VWAP, worst price, and total cost
```

Bundle quantity:

```text
bundle_qty = min(executable_qty_lots across all required legs)
```

If any leg cannot fill the required quantity, the bundle is rejected:

```text
RejectedInsufficientDepth
```

This prevents a deep leg from hiding a shallow leg. `OpportunityIntent` carries
the final `bundle_qty` so downstream Risk can decide whether the opportunity is
large enough to matter.

## 11. VWAPPrecheck

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

Signal v0.5 supports BUY-leg pricing. SELL legs are structurally represented
but rejected by `PriceVectorBuilder` / `VWAPPrecheck` unless SELL pricing is
explicitly enabled after the cost/proceeds convention is approved. The runtime
must not silently treat SELL as negative cost.

## 12. Unit Edge vs Total Edge

`TheoreticalEdgeCalculator` computes unit edge, total edge, and edge bps.

```text
unit_edge_tick =
    guaranteed_payout_per_bundle_tick
    - vwap_cost_per_bundle_tick
    - fee_per_bundle_tick
    - latency_buffer_per_bundle_tick
    - slippage_buffer_per_bundle_tick

total_edge_tick = unit_edge_tick * bundle_qty
```

The v0.5 `FeeModel` returns the configured default fee.

The v0.5 `LatencyBufferModel` returns the configured default latency buffer.

The v0.5 slippage buffer model is fixed-config.

Passing conditions:

```text
unit_edge_tick >= min_unit_edge_tick
total_edge_tick >= min_total_edge_tick
edge_bps >= min_edge_bps
bundle_qty >= min_bundle_qty
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

This split matters because a high unit edge with tiny size may be unusable, and
a large total edge with low bps may not justify risk or latency.

## 13. Intent Lifecycle

`IntentBuilder` turns a priced bundle into a complete `OpportunityIntent`.

Every `PaperOpportunity` should carry:

```text
created_ts_ns
expires_at_ns
idempotency_key
proof_ref
oracle_artifact_hash
constraint_hash
bundle_hash
snapshot_version
snapshot_version_hash
bundle_qty
unit_edge_tick
total_edge_tick
edge_bps
```

The default workflow uses deterministic timestamps for fixture determinism.
Runtime scans should set a real monotonic timestamp and a nonzero TTL before a
later Risk layer can treat the intent as fresh.

Expired intents must not be converted into execution requests.

## 14. Idempotency

Each paper opportunity has an `idempotency_key` derived from stable evidence,
including bundle identity, bundle hash, snapshot hash, bundle quantity, and unit
edge.

The key is used for idempotent publication. Repeated scans of the same
opportunity should not create a stream of duplicate opportunities.

The idempotency key is evidence for dedupe and audit. It is still not an order
ID.

## 15. Deduplication

`IntentDeduper` suppresses repeated `PaperOpportunity` intents within a TTL.

Behavior:

```text
first key within TTL      -> publish
same key within TTL       -> rejected_duplicate
same key after TTL expiry -> publish again
different key             -> publish
```

Duplicate rejections are counted but normally not published as rejection
intents.

## 16. Rate Limiting

`IntentRateLimiter` limits how many intents can be published per second.

Current v0.5 behavior is a simple per-second counter:

```text
under max_intents_per_second -> publish
over max_intents_per_second  -> rejected_rate_limited
next second                  -> counter resets
```

Rate-limited intents are counted but not published.

## 17. Ranking

`OpportunityRanker` sorts deterministically:

```text
1. PaperOpportunity before rejection
2. total_edge_tick descending
3. edge_bps descending
4. bundle_qty descending
5. bundle_id ascending
6. intent_id ascending
```

This keeps output ordering stable across repeated scans with identical inputs.

## 18. Publishing

Publishing is synchronous in v0.5.

Available publishers:

- `CapturingIntentPublisher` for tests
- `JsonlIntentWriter` for tool output
- `PaperIntentPublisher` for paper-path integration

Publishing does not call Execution. It writes or captures `OpportunityIntent`
records only.

## 19. Metrics

Signal v0.5 exposes lightweight counters and a scan latency placeholder through
`SignalMetrics`. This is deliberately not a full metrics framework.

Counters:

```text
signal.scan.count
signal.bundle.scanned
signal.bundle.rejected
signal.bundle.passed
signal.reject.settled
signal.reject.missing_snapshot
signal.reject.stale_lob
signal.reject.snapshot_skew
signal.reject.insufficient_depth
signal.reject.edge_below_threshold
signal.reject.duplicate
signal.reject.rate_limited
signal.intent.published
```

Latency placeholder:

```text
signal.scan.latency_ns
  count
  last
  min
  max
```

These metrics are printed by `verify_signal_workflow` and can later be wired to
runtime telemetry without changing the scan loop semantics.

## 20. Workflow Verification

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

snapshot:
  consistency_checked:
  rejected_snapshot_skew:
  rejected_stale_snapshot:

settlement:
  settlement_masks_available:
  rejected_invalid_settlement:

vwap:
  vwap_checked:
  bundle_qty_min:
  bundle_qty_max:
  enough_depth:
  insufficient_depth:

edge:
  edge_computed:
  unit_edge_min:
  unit_edge_max:
  total_edge_min:
  total_edge_max:
  edge_bps_min:
  edge_bps_max:
  above_threshold:
  below_threshold:

intents:
  paper_opportunities:
  rejected_invalid_settlement:
  rejected_bad_market_state:
  rejected_insufficient_depth:
  rejected_low_edge:
  duplicate_intents:
  rejected_duplicate:
  rate_limited:
  rejected_rate_limited:
  intents_published:

intent_lifecycle:
  intents_with_expiry:
  intents_with_idempotency_key:
  duplicate_rejected:
  rate_limited:

metrics:
  signal.scan.count:
  signal.bundle.scanned:
  signal.bundle.rejected:
  signal.bundle.passed:
  signal.reject.settled:
  signal.reject.missing_snapshot:
  signal.reject.stale_lob:
  signal.reject.snapshot_skew:
  signal.reject.insufficient_depth:
  signal.reject.edge_below_threshold:
  signal.reject.duplicate:
  signal.reject.rate_limited:
  signal.intent.published:
  signal.scan.latency_ns:

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
vwap_checked: 1
enough_depth: 1
edge_computed: 1
paper_opportunities: 1
rejected_insufficient_depth: 0
intents_published: 1
bundle_qty_min: 20
bundle_qty_max: 20
total_edge_max: 3800000
determinism_passed: true
```

The emitted JSONL intent must contain:

```text
"status":"PaperOpportunity"
"enough_depth":true
"bundle_qty":20
"estimated_edge_tick":3800000
```

## 21. Failure Conditions

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
duplicate PaperOpportunity repeatedly published within dedupe TTL
rate-limited PaperOpportunity still published
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
DuplicateIntent
```

Successful paper status:

```text
PaperOpportunity
```

Initial status before processing:

```text
CandidateOnly
```

## 22. Runtime Boundary

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

Risk Engine must approve any intent before execution. Execution must never treat
a raw `OpportunityIntent` as an order.

## 23. Known Limitations

Current v0.5 limitations:

- BUY-leg VWAP is supported.
- Signal v0.5 does not support SELL leg pricing unless explicitly enabled after
  cost/proceeds semantics are settled.
- Fee model is fixed-config, not venue-specific.
- Latency buffer model is fixed-config, not dynamic.
- Slippage buffer model is fixed-config, not dynamic.
- Settlement mask support is minimal.
- Fixture workflow proves rejection and positive paper-intent determinism, not
  positive live profitability.
- No live market scan loop is wired to execution.
- No position, inventory, or exposure constraints are evaluated here.
- No API / LLM / solver access is available in runtime.

These are deliberate boundaries for the current Signal v0.5.

## 24. Next Layer: Risk Engine

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

# Market State Workflow v1

Status: accepted

## Purpose

Market State Workflow v1 verifies that the market state layer can produce a
deterministic, queryable `MarketStateSnapshot` from both Polymarket WS book
data and synthetic Polygon confirmed fill data.

This checkpoint does not validate strategy value, order execution, live chain
parity, or live network stability.

## Workflow Paths

### Workflow A: WS Book Path

```text
market_39.raw
  -> RawLogReader
  -> DecodeInputAdapter
  -> DecodePipeline
  -> MarketStateEventAdapter
  -> MarketStateStore
  -> MarketStateView.get_snapshot()
```

Acceptance:

```text
packets_read = 39
normalized_events = 39
snapshot_events = 1
delta_events = 35
heartbeat_events = 3
decode_errors = 0
normalization_errors = 0
state_errors = 0
entity_count = 1
legacy_book_hash = 12959912045291989833
snapshot_ok = true
has_book = true
```

### Workflow B: Chain Fill Path

```text
synthetic ClassifiedFill
  -> ChainConfirm classification record
  -> MarketStateEventAdapter
  -> MarketStateStore
  -> MarketStateView.get_snapshot()
```

Synthetic fixture includes:

```text
1 buy aggressor fill
1 sell aggressor fill
1 ambiguous fill
1 removed fill
```

Acceptance:

```text
classified_fills = 4
buy_aggressor_fills = 1
sell_aggressor_fills = 1
ambiguous_fills = 1
removed_fills = 1
chain_fills_applied = 3
has_chain_state = true
```

Hard invariant:

```text
unknown / ambiguous fills do not enter buy/sell volume
removed fills do not remain in confirmed flow
```

### Workflow C: Combined Path

```text
market_39.raw + synthetic chain fills
  -> MarketStateStore
  -> MarketStateSnapshot(book + chain + quality)
```

Acceptance:

```text
legacy_book_hash = 12959912045291989833
chain_hash != 0
combined_state_hash != 0
snapshot has book fields
snapshot has chain fields
snapshot has quality fields
```

Hard invariant:

```text
chain fill does not change legacy_book_hash
chain fill does not change best_bid / best_ask / depth
chain fill changes only confirmed trade state
```

## Fixtures

WS fixture:

```text
tests/fixtures/polymarket/market_39.raw
```

Auxiliary JSONL:

```text
tests/fixtures/polymarket/market_39.jsonl
```

Chain fixture:

```text
tests/fixtures/chain_confirm/synthetic_order_filled.jsonl
```

## Baselines

Command:

```text
build/verify_market_state_workflow \
  --raw tests/fixtures/polymarket/market_39.raw \
  --chain-fixture tests/fixtures/chain_confirm/synthetic_order_filled.jsonl \
  --repeat 10000 \
  --check-determinism
```

Output baseline:

```text
ws_path:
  packets_read: 39
  normalized_events: 39
  snapshot_events: 1
  delta_events: 35
  heartbeat_events: 3
  decode_errors: 0
  normalization_errors: 0

chain_path:
  chain_logs: 4
  classified_fills: 4
  buy_aggressor_fills: 1
  sell_aggressor_fills: 1
  unknown_fills: 0
  ambiguous_fills: 1
  removed_fills: 1

state_path:
  book_snapshots_applied: 1
  book_deltas_applied: 35
  chain_fills_applied: 3
  state_errors: 0
  entity_count: 1

snapshot_output:
  snapshot_ok: true
  has_book: true
  has_chain_state: true
  has_quality_state: true
  usable_for_depth: true
  usable_for_signal: true

hashes:
  legacy_book_hash: 12959912045291989833
  chain_hash: 5495362919752233551
  combined_state_hash: 18417937017309501702
  determinism_passed: true
```

CTest baseline:

```text
ctest: 190 passed
LiveFeedSmokeManual: disabled
```

## Hash Model

`legacy_book_hash` is the old book-only deterministic state hash:

```text
legacy_book_hash = 12959912045291989833
```

This must remain stable when chain fills are applied.

`chain_hash` summarizes confirmed trade fields in the published snapshot:

```text
chain_hash = 5495362919752233551
```

`combined_state_hash` combines book and chain state:

```text
combined_state_hash = 18417937017309501702
```

Future changes may intentionally alter `chain_hash` or
`combined_state_hash`, but `legacy_book_hash` must not change unless book
semantics or hash inputs intentionally change.

## Failure Conditions

Any of these fail the workflow:

```text
decode_errors > 0
normalization_errors > 0
state_errors > 0
legacy_book_hash != 12959912045291989833
unknown fill counted as buy/sell
ambiguous fill counted as buy/sell
removed fill remains in confirmed flow
chain fill changes book hash
chain fill changes book depth or BBO
snapshot missing book state
snapshot missing chain state
snapshot missing quality state
workflow determinism fails
```

## What v1 Proves

Market State Workflow v1 proves:

```text
WS can reconstruct book state.
Chain synthetic fill can update confirmed trade state.
Book state and chain state are separated.
MarketStateView can output a combined snapshot.
Hashing distinguishes book / chain / combined state.
Workflow-level determinism holds.
```

This is stronger than component-level success: the full workflow produces an
effective market state snapshot.

## What v1 Does Not Prove

This checkpoint does not prove:

```text
real Polygon live logs are complete
WS live stream and chain live stream reconcile over time
multi-market / multi-asset state cannot leak
real OrderFilled classification coverage is sufficient
latency is stable under live network conditions
the state has predictive value for strategy
```

## Next Validation Step

Next phase:

```text
Step 11: Live-like WS + Polygon validation
```

Goals:

```text
real streams produce valid MarketStateSnapshot continuously
WS and Chain paths can be compared over live-like block ranges
quality gate degrades state correctly when chain or WS quality drops
no Signal / Risk / Execution integration yet
```

Suggested tools:

```text
run_market_state_live_smoke
chain_ws_http_parity_smoke
multi_asset_synthetic_workflow
```

Do not proceed directly to SignalEngine from this checkpoint.

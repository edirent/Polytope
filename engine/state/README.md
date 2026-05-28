# Market State Layer Technical Document

Status: v1 accepted
Module: `engine/state`
Purpose: produce deterministic, queryable, quality-aware market state from Polymarket WS book data and Polygon-confirmed fill data.

---

## 1. Executive Summary

The Market State Layer is the central state construction layer of the system.

Its responsibility is to transform normalized market-data events and confirmed chain fill records into a stable `MarketStateSnapshot` that can be safely consumed by future strategy, research, paper-trading, and monitoring components.

The layer combines two different data sources with different semantics:

```text
Polymarket CLOB WebSocket
  -> fast order book / liquidity state

Polygon OrderFilled logs
  -> confirmed trade / taker-side truth
```

The layer does not treat WS-derived trade direction as truth. WS is used for current displayed liquidity. Polygon confirmed fills are used for confirmed trade facts. The two sources are merged only at the state layer through explicit book state, confirmed trade state, and data-quality state.

The main invariant is:

```text
WS book updates can modify LOB state.
Polygon confirmed fills can modify confirmed trade state.
Polygon confirmed fills must not directly mutate LOB depth, BBO, or legacy book hash.
```

---

## 2. Scope

### 2.1 In Scope

The Market State Layer is responsible for:

1. Applying WS-derived book snapshots and book deltas.
2. Maintaining per-asset order book state.
3. Maintaining best bid, best ask, spread, mid, and depth.
4. Maintaining confirmed trade state from Polygon-confirmed fills.
5. Tracking unknown, ambiguous, removed, and confirmed fill states.
6. Maintaining market quality status such as stale, recovering, crossed, chain-lagging, and chain-mismatch.
7. Maintaining settlement state skeletons.
8. Producing copy-out `MarketStateSnapshot` objects.
9. Preserving deterministic book hash behavior.
10. Separating book hash, chain hash, and combined state hash.
11. Providing read-only state access through `MarketStateView`.

### 2.2 Out of Scope

The Market State Layer does not:

1. Connect to WebSocket directly.
2. Decode JSON.
3. Decode Polygon logs.
4. Classify raw `OrderFilled` logs.
5. Infer trade direction from WS book changes.
6. Generate trading signals.
7. Run strategy logic.
8. Manage risk.
9. Submit orders.
10. Execute live trades.
11. Perform portfolio accounting.
12. Prove strategy profitability.
13. Prove live Polygon log completeness.
14. Prove live WS / chain parity over long time periods.

Those belong to Feed, Decode, ChainConfirm, Signal, Risk, Execution, or future validation layers.

---

## 3. Upstream and Downstream Boundaries

### 3.1 Upstream: WS Book Path

```text
RawLogReader / LiveFeed
  -> RawPacket
  -> DecodeInputAdapter
  -> DecodePipeline
  -> NormalizedEventBatch
  -> MarketStateEventAdapter
  -> MarketStateStore
```

WS-derived normalized events are allowed to update the book.

Examples:

```text
Snapshot event -> book replacement
Delta event    -> price level update
Heartbeat      -> liveness / ignored for book mutation
Lifecycle      -> closed / resolved state
```

### 3.2 Upstream: Polygon Chain Path

```text
Polygon EthLog
  -> EthLogDecoder
  -> OrderFilledDecoder
  -> OrderFilledClassifier
  -> ClassifiedFillRecord
  -> MarketStateEventAdapter
  -> MarketStateStore
```

Polygon-confirmed fills are allowed to update confirmed trade state, but not LOB state.

Examples:

```text
Confirmed buy aggressor fill  -> confirmed buy flow
Confirmed sell aggressor fill -> confirmed sell flow
Unknown fill                  -> unknown count
Ambiguous fill                -> ambiguous count
Removed fill                  -> removed / reverted state
```

### 3.3 Downstream

The downstream consumer should read only:

```text
MarketStateView
  -> MarketStateSnapshot
```

Future strategy code must not directly consume:

```text
RawPacket
NormalizedEvent
EthLog
OrderFilledEvent
PendingTradeHint
EntityStateStore internals
ConfirmedFillStore internals
```

---

## 4. High-Level Architecture

```text
engine/state
├── EntityStateStore
├── StateHasher
├── MarketStateStore
├── MarketStateEvent
├── MarketStateEventAdapter
│
├── book/
│   ├── LOBWriter
│   ├── BookState
│   └── OrderBook
│
├── chain/
│   ├── ConfirmedTradeState
│   ├── ChainStateWriter
│   ├── SettlementState
│   └── GlobalBitmaskState
│
├── quality/
│   ├── BookQualityState
│   ├── DataQualityGate
│   └── ReconciliationState
│
├── snapshot/
│   └── SnapshotPublisher
│
├── shard/
│   ├── LOBShard
│   └── ShardRouter
│
└── view/
    ├── MarketStateView
    ├── MarketStateSnapshot
    └── MarketStateQueryResult
```

The current implementation is v1. It uses one logical shard:

```text
NUM_SHARDS = 1
ShardRouter(asset_id) -> 0
```

The single-shard design preserves the future model of one writer per market / asset while avoiding unnecessary concurrency complexity at this stage.

---

## 5. Core Design Principles

### 5.1 WS Maintains Book State

WS data is used for:

```text
best bid
best ask
depth
spread
mid
book version
book freshness
recovering / crossed state
```

WS data is not used as confirmed trade direction.

### 5.2 Polygon Maintains Confirmed Trade State

Polygon `OrderFilled` classification is used for:

```text
confirmed buy aggressor flow
confirmed sell aggressor flow
unknown fill count
ambiguous fill count
removed fill count
last confirmed trade
chain hash
```

Polygon fills do not directly mutate:

```text
best bid
best ask
depth
LOB price levels
legacy book hash
```

### 5.3 Unknown Is Better Than Wrong

If a fill cannot be safely classified, it remains:

```text
Unknown
Ambiguous
Invalid
RemovedByReorg
```

It must not be forced into buy or sell volume.

### 5.4 Copy-Out Snapshots Only

`MarketStateView` returns copy-out snapshots. It does not expose internal maps, vectors, mutable references, or raw book structures.

This protects future migration to:

```text
Double Buffer
RCU Snapshot
Seqlock
Multi-shard state
```

### 5.5 Hash Separation

The state layer maintains separate hash concepts:

```text
legacy_book_hash
chain_hash
combined_state_hash
```

`legacy_book_hash` must remain stable for WS-only book replay.

Chain fills may change `chain_hash` and `combined_state_hash`, but must not change `legacy_book_hash`.

---

## 6. MarketStateEvent

`MarketStateEvent` is the unified input to the State Layer.

It represents both WS-derived events and chain-derived events.

### 6.1 Event Types

```cpp
enum class MarketStateEventType : uint8_t {
    WsBookSnapshot,
    WsBookDelta,
    WsHeartbeat,
    WsLifecycle,

    ChainConfirmedFill,
    ChainRemovedFill,
    ChainSettlement,

    DataQualityUpdate
};
```

### 6.2 Responsibilities

The adapter layer converts:

```text
NormalizedEventBatch -> MarketStateEvent[]
ClassifiedFillRecord -> MarketStateEvent
```

The adapter must preserve:

```text
market_id
asset_id
recv time
source sequence
event type
```

The adapter must not:

```text
mutate book
classify chain direction
infer WS trade direction
modify confirmed trade state
```

It is only a boundary translation layer.

---

## 7. Book State

### 7.1 Order Book Model

The book state tracks current displayed liquidity.

```cpp
struct PriceLevel {
    int64_t price_tick;
    int64_t size_lots;
};

constexpr int MAX_DEPTH = 64;

struct OrderBook {
    std::string market_id;
    std::string asset_id;

    uint64_t version;

    PriceLevel bids[MAX_DEPTH];
    PriceLevel asks[MAX_DEPTH];

    uint32_t bid_count;
    uint32_t ask_count;

    bool has_bid;
    bool has_ask;

    int64_t best_bid_tick;
    int64_t best_ask_tick;

    bool crossed;
    bool recovering;

    uint64_t last_ws_recv_ns;
    uint64_t last_ws_packet_id;
};
```

### 7.2 Book Update Rules

```text
Snapshot:
  replace full book
  mark entity live
  increment book version

Delta:
  only apply after snapshot
  size_lots == 0 removes level
  size_lots > 0 upserts level
  increment book version

Delta before snapshot:
  mark recovering or reject
  do not mutate book

Heartbeat:
  no book mutation

Lifecycle:
  mark closed / resolved if applicable
```

### 7.3 Book Invariants

After each book mutation:

```text
bids sorted descending
asks sorted ascending
size_lots >= 0
zero-size levels are not retained
best_bid = first bid level
best_ask = first ask level
best_bid > best_ask implies crossed book
delta before snapshot does not corrupt book
```

---

## 8. LOBWriter

`LOBWriter` is the formal writer for WS book updates.

It wraps the existing `EntityStateStore` behavior rather than rewriting book logic.

### 8.1 Allowed Inputs

```text
WsBookSnapshot
WsBookDelta
WsHeartbeat
WsLifecycle
```

### 8.2 Ignored Inputs

```text
ChainConfirmedFill
ChainRemovedFill
ChainSettlement
```

### 8.3 Required Behavior

```text
WS snapshot updates book.
WS delta updates book.
WS heartbeat does not change book version.
Chain fill does not change book state.
Chain fill does not change legacy_book_hash.
```

---

## 9. ConfirmedTradeState

`ConfirmedTradeState` tracks Polygon-confirmed fill facts.

### 9.1 State Model

```cpp
enum class AggressorSide : uint8_t {
    Unknown,
    Buy,
    Sell
};

struct ConfirmedTradeState {
    uint64_t version;

    uint64_t last_block_number;
    uint64_t last_chain_seen_ns;

    bool has_last_trade;
    int64_t last_trade_price_tick;
    int64_t last_trade_size_lots;
    AggressorSide last_taker_side;

    int64_t confirmed_buy_lots_2s;
    int64_t confirmed_sell_lots_2s;

    int64_t confirmed_buy_lots_10s;
    int64_t confirmed_sell_lots_10s;

    uint32_t unknown_fill_count_recent;
    uint32_t ambiguous_fill_count_recent;
    uint32_t removed_fill_count_recent;
};
```

### 9.2 Update Rules

```text
BuyAggressor:
  increase confirmed buy volume
  update last confirmed trade

SellAggressor:
  increase confirmed sell volume
  update last confirmed trade

Unknown:
  increase unknown count
  do not increase buy/sell volume

Ambiguous:
  increase ambiguous count
  do not increase buy/sell volume

RemovedByReorg:
  increase removed count
  remove or invalidate prior contribution if previously counted
```

### 9.3 Hard Invariant

`ConfirmedTradeState` must not modify:

```text
OrderBook
best_bid
best_ask
depth
legacy_book_hash
```

---

## 10. SettlementState and GlobalBitmaskState

Settlement state is currently a minimal skeleton.

It exists to support later market-resolution and candidate-bundle logic.

### 10.1 SettlementState

```cpp
enum class SettlementStatus : uint8_t {
    Unknown,
    Open,
    Closed,
    Resolved
};

struct SettlementState {
    SettlementStatus status;
    bool resolved;
    std::string winning_asset_id;
    uint64_t last_update_block;
    uint64_t version;
};
```

### 10.2 GlobalBitmaskState

```cpp
struct GlobalBitmaskState {
    uint64_t resolved_true_mask;
    uint64_t resolved_false_mask;
    uint64_t invalid_mask;
    uint64_t version;
};
```

### 10.3 Current Limitation

This layer does not yet connect to:

```text
Oracle compiler
Candidate bundles
Signal engine
Risk engine
Execution
```

---

## 11. BookQualityState

`BookQualityState` determines whether current state should be considered usable.

### 11.1 Quality Model

```cpp
enum class BookQuality : uint8_t {
    Unknown,
    Good,
    Stale,
    Recovering,
    Crossed,
    ChainMismatch,
    ChainLagging,
    Closed,
    Resolved
};

struct BookQualityState {
    BookQuality quality;

    bool ws_live;
    bool chain_live;

    uint64_t last_ws_recv_ns;
    uint64_t last_chain_seen_ns;

    uint32_t ws_decode_errors_recent;
    uint32_t state_errors_recent;
    uint32_t chain_decode_errors_recent;

    uint32_t chain_ws_mismatch_count_recent;

    bool usable_for_depth;
    bool usable_for_signal;
};
```

### 11.2 Quality Rules

```text
No snapshot / delta before snapshot -> Recovering
best_bid > best_ask                 -> Crossed
WS age too high                     -> Stale
chain lag too high                  -> ChainLagging
chain / WS reconciliation mismatch  -> ChainMismatch
closed                              -> Closed
resolved                            -> Resolved
otherwise                           -> Good
```

### 11.3 Depth vs Signal

Depth and signal quality are intentionally separate.

Example:

```text
WS book healthy, chain lagging:
  usable_for_depth = true
  usable_for_signal = false

WS stale:
  usable_for_depth = false
  usable_for_signal = false

Book crossed:
  usable_for_depth = false
  usable_for_signal = false
```

This prevents the system from throwing away useful depth information while also preventing confirmed-flow-dependent strategies from running under bad chain quality.

---

## 12. SnapshotPublisher

`SnapshotPublisher` publishes copy-out state snapshots.

The current intended model is double-buffered publication.

### 12.1 Writer Behavior

```text
1. Select inactive slot.
2. Fill full MarketStateSnapshot.
3. Publish via release-store active index.
```

### 12.2 Reader Behavior

```text
1. Acquire-load active index.
2. Copy snapshot out.
3. Return StateQueryResult<MarketStateSnapshot>.
```

### 12.3 Required Properties

```text
reader never receives mutable internal reference
missing asset returns MissingEntity
second publish increments version
snapshot includes book + chain + quality state
```

---

## 13. MarketStateSnapshot

`MarketStateSnapshot` is the final output of the Market State Layer.

It must include identity, versions, book state, confirmed chain state, settlement state, quality state, and hashes.

### 13.1 Required Fields

```text
identity:
  market_id
  asset_id

versions:
  state_version
  book_version
  chain_version
  settlement_version

book:
  has_bid
  has_ask
  crossed
  best_bid_tick
  best_ask_tick
  mid_tick
  spread_tick
  bid_count
  ask_count
  bids[64]
  asks[64]

chain:
  has_confirmed_trade
  last_confirmed_trade_price_tick
  last_confirmed_trade_size_lots
  last_confirmed_taker_side
  confirmed_buy_lots_2s
  confirmed_sell_lots_2s
  confirmed_buy_lots_10s
  confirmed_sell_lots_10s
  unknown_fill_count_recent
  ambiguous_fill_count_recent
  removed_fill_count_recent

settlement:
  settlement_status
  resolved
  winning_asset_id

quality:
  book_quality
  usable_for_depth
  usable_for_signal
  last_ws_recv_ns
  last_chain_seen_ns

hash:
  legacy_book_hash
  chain_hash
  combined_state_hash
```

### 13.2 Valid Snapshot Definition

A snapshot is considered structurally valid when:

```text
snapshot_ok = true
market_id / asset_id are non-empty
state_version > 0
book_version > 0 after snapshot
legacy_book_hash != 0
quality state is present
```

A snapshot is depth-usable when:

```text
usable_for_depth = true
not recovering
not stale
not crossed
not closed / resolved
at least one side of the book exists
```

A snapshot is signal-usable only when:

```text
usable_for_signal = true
usable_for_depth = true
chain state is live enough
no severe chain gap
unknown / ambiguous fill ratio is below threshold
```

---

## 14. MarketStateView

`MarketStateView` is the public read-only interface.

It should read published snapshots, not mutable internal book state.

### 14.1 Responsibilities

```text
return copy-out MarketStateSnapshot
return best bid / best ask
return BBO
return mid / spread
return query errors
hide shard internals
hide mutable store internals
```

### 14.2 Non-Responsibilities

`MarketStateView` must not:

```text
decode raw payloads
classify chain fills
infer WS direction
modify book
modify confirmed trade state
submit orders
run strategy
```

---

## 15. Hash Model

### 15.1 Legacy Book Hash

`legacy_book_hash` is the deterministic book-only hash inherited from earlier Feed/State E2E.

Current baseline:

```text
legacy_book_hash = 12959912045291989833
```

This hash must remain unchanged when synthetic chain fills are applied.

### 15.2 Chain Hash

`chain_hash` summarizes confirmed trade state in the published snapshot.

Current baseline for synthetic workflow:

```text
chain_hash = 5495362919752233551
```

### 15.3 Combined State Hash

`combined_state_hash` combines book and chain state.

Current baseline for synthetic workflow:

```text
combined_state_hash = 18417937017309501702
```

Future changes may intentionally alter `chain_hash` or `combined_state_hash`, but `legacy_book_hash` must not change unless book semantics or book hash inputs intentionally change.

---

## 16. Workflow Validation

### 16.1 WS Book Path

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

### 16.2 Chain Fill Path

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

### 16.3 Combined Path

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

---

## 17. Workflow Verification Tool

The primary workflow verification tool is:

```text
verify_market_state_workflow
```

Example command:

```text
build/verify_market_state_workflow \
  --raw tests/fixtures/polymarket/market_39.raw \
  --chain-fixture tests/fixtures/chain_confirm/synthetic_order_filled.jsonl \
  --repeat 10000 \
  --check-determinism
```

Expected output blocks:

```text
ws_path
chain_path
state_path
snapshot_output
hashes
latency
```

Baseline output:

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

---

## 18. Failure Conditions

Any of the following fail the workflow:

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

---

## 19. Tests

Current workflow tests include:

```text
ws_book_to_snapshot_test
chain_fill_to_snapshot_test
combined_market_state_workflow_test
```

These verify:

```text
WS replay produces snapshot
Synthetic chain fill produces confirmed trade state
Combined book + chain workflow produces full snapshot
legacy_book_hash remains stable
chain state does not mutate book
unknown / ambiguous / removed fills do not pollute confirmed flow
```

Component tests cover lower-level behavior of decode, classification, state mutation, and query surfaces.

---

## 20. Observability

Workflow-level summaries should report:

```text
feed_decode_state_summary:
  packets_read
  normalized_events
  decode_errors
  normalization_errors

book_state_summary:
  book_snapshots_applied
  book_deltas_applied
  heartbeats_ignored
  state_errors
  legacy_book_hash

chain_state_summary:
  fills_classified
  buy_aggressor_fills
  sell_aggressor_fills
  unknown_fills
  ambiguous_fills
  removed_fills
  chain_hash

quality_summary:
  good_count
  stale_count
  recovering_count
  crossed_count
  chain_lagging_count
  chain_mismatch_count
  usable_for_depth_count
  usable_for_signal_count

snapshot_summary:
  snapshots_published
  snapshots_read
  missing_snapshot_reads
  invalid_snapshot_reads
```

Without a workflow summary, debugging state failures becomes unnecessarily expensive.

---

## 21. Latency

The workflow verification tool may report latency, but v1 does not enforce hard performance thresholds.

Relevant latency groups:

```text
adapter_ns
decode_pipeline_ns
state_event_adapter_ns
lob_apply_ns
chain_apply_ns
quality_update_ns
snapshot_publish_ns
view_snapshot_read_ns
total_ns
```

Latency should be grouped by:

```text
ws_snapshot
ws_delta
ws_heartbeat
chain_confirmed_fill
chain_removed_fill
```

The current priority is correctness and state separation. Performance thresholds should be added only after live-like validation establishes realistic runtime behavior.

---

## 22. Live-Like Validation

Market State Workflow v1 does not prove live stream completeness.

The next validation phase is:

```text
Step 11: Live-like WS + Polygon validation
```

The intended tools are:

```text
run_market_state_live_smoke
chain_ws_http_parity_smoke
multi_asset_synthetic_workflow
```

Current status:

```text
offline workflow: accepted
synthetic multi-asset workflow: accepted
real network smoke: pending external env and asset id
```

Live-like validation should prove:

```text
real streams produce valid MarketStateSnapshot continuously
WS and Chain paths can be compared over live-like block ranges
quality gate degrades state correctly when chain or WS quality drops
no Signal / Risk / Execution integration yet
```

---

## 23. Current Accepted Baselines

Build and test:

```text
ctest: 190 passed
LiveFeedSmokeManual: disabled
```

Market State Workflow:

```text
legacy_book_hash = 12959912045291989833
chain_hash = 5495362919752233551
combined_state_hash = 18417937017309501702
determinism_passed = true
```

Step 11 synthetic validation:

```text
multi_asset_synthetic_workflow:
  asset_a snapshot_ok = true
  asset_a chain_state_ok = true
  asset_a book_hash_unchanged = true
  asset_b ambiguous_not_counted = true
  asset_c recovering = true
  asset_isolation_ok = true
```

Manual live tests are registered but disabled by default:

```text
MarketStateLiveSmokeManual
ChainWsHttpParitySmokeManual
LiveFeedSmokeManual
```

---

## 24. Known Limitations

The current layer does not yet prove:

```text
real Polygon live logs are complete
WS live stream and chain live stream reconcile over time
real OrderFilled classification coverage is sufficient
multi-market live state cannot leak
latency is stable under live network conditions
state has predictive value for strategy
```

These are validation or strategy questions, not State v1 correctness questions.

---

## 25. Done Definition

Market State Workflow v1 is considered accepted when:

```text
1. WS replay can produce a valid MarketStateSnapshot.
2. Synthetic chain fill can update ConfirmedTradeState.
3. Combined workflow outputs book + chain + quality.
4. legacy_book_hash remains 12959912045291989833.
5. chain fill does not modify LOB depth, BBO, or book hash.
6. unknown / ambiguous / removed fills do not pollute confirmed buy/sell flow.
7. verify_market_state_workflow outputs a complete summary.
8. workflow determinism passes.
9. ctest passes.
```

This means the system has a valid Market State Layer output, but it does not yet imply that the state is profitable, predictive, or live-production ready.

---

## 26. Next Step

The correct next step is not SignalEngine.

The next step is:

```text
Live-like validation:
  real WS stream
  real Polygon log stream
  HTTP backfill parity
  quality degradation behavior
```

Only after this phase should a paper-only SignalEngine consume `MarketStateView`.

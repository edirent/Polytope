# Market Making Module

## Executive Summary

The market making module is a strategy-layer component that turns a healthy
`MarketDepthView` plus current inventory into `QuoteIntent` and
`CancelQuoteIntent` objects. It does not submit orders, approve risk, connect to
authenticated APIs, or mutate paper/live positions.

## Scope

v0 includes:

- mid-price fair value baseline
- fixed-buffer spread model
- inventory skew model
- conservative quote sizing
- quote price clamping
- TTL, fair-move, inventory-move, and bad-book refresh policy
- active quote state tracking
- deterministic workflow verifier

## Non-Scope

The module does not include live maker order placement, Polymarket authenticated
API calls, optimistic paper fills, automatic market selection, or frontend
controls. Dashboard integration remains out of scope for maker PnL.

## Data Flow

```text
MarketDepthView + inventory
  -> FairValueModel
  -> SpreadModel
  -> InventorySkewModel
  -> QuoteSizeModel
  -> QuoteEngine
  -> QuoteRefreshPolicy
  -> QuoteIntent / CancelQuoteIntent
```

## Fair Value

v0 fair value is the current mid price:

```text
fair_value_tick = (best_bid_tick + best_ask_tick) / 2
```

The model refuses to quote missing bid/ask books, stale books, recovering books,
crossed books, closed markets, and resolved markets.

## Quote Formula

```text
bid = fair_value - half_spread - inventory_skew
ask = fair_value + half_spread - inventory_skew
```

Long inventory produces positive skew and shifts both bid and ask down. This
reduces additional buying and encourages selling.

## Latency-Risk Defaults

The defensive default profile is intentionally wider and smaller than the early
prototype settings:

- `min_half_spread_tick = 25_000`, so passive fills must pay more edge before
  we accept adverse-selection risk.
- `base_quote_size_lots = 9`, with quote-risk test/probe notional capped at
  `10_000_000` ticks per quote.
- `max_inventory_skew_tick = 75_000`, so inventory quickly shifts quotes toward
  flattening. The paper/probe quote-risk policy permits a controlled negative
  edge floor for this inventory-dump path.

## Verification

```bash
build/verify_market_making_workflow --check-determinism --require-quote
```

The verifier prints quote intents, cancel intents, active quote count, quote
spread/skew summary, zeroed maker-PnL fields for v0, and a deterministic module
hash.

## Runtime Boundary

`QuoteIntent` is not an order. It must be approved by future quote-risk logic and
handled by a paper/live execution adapter before any fill can exist.

## Quote Risk

`QuoteRiskEvaluator` is the approval boundary for market-making quotes. It
consumes `QuoteIntent`, `MarketDepthView`, current inventory/exposure inputs, and
quote-specific policy fields. It returns `QuoteRiskDecision` and, only on
approval, an `ApprovedQuote`.

`ApprovedQuote` is not an order. It preserves the approved quote legs and evidence
hashes so a future paper maker execution adapter can consume them, but it does
not submit, cancel, replace, or mutate ledger state.

The evaluator is pure in v0:

- no execution calls
- no network access
- no paper ledger mutation
- no reservation creation
- no feed/decode/chain-confirm internals

`MarketMakingEngine` may propose quotes, but it must not bypass quote risk before
any maker execution path is introduced.

## Paper Maker Execution

`PaperMakerExecutionAdapter` is the paper-only maker execution boundary for
approved market-making quotes. It consumes `ApprovedQuote`, stores active paper
quotes in `ActivePaperQuoteBook`, and emits `MakerExecutionReport` records when a
paper fill condition is met.

The adapter supports three deterministic fill modes:

- `NoFill`: stores quotes but never fills them.
- `Conservative`: fills only with explicit trade-through evidence. A bid fills
  when `trade_price_tick <= bid.price_tick`; an ask fills when
  `trade_price_tick >= ask.price_tick`.
- `BookCross`: fills from visible crossed depth. A bid fills when best ask is at
  or below the bid price; an ask fills when best bid is at or above the ask
  price.

`ActivePaperQuoteBook` replaces quotes with the same `quote_group_id`, ignores
duplicate `idempotency_hash` values, supports cancel-by-group, expires old
quotes, and queries active quotes by `asset_index`.

This path is intentionally simulation-only:

- no live order submission
- no authenticated API access
- no wallet, signer, or private key
- no network calls
- no `PaperLedger` mutation

Paper maker reports are observations for the paper accounting layer. The
execution adapter does not mutate `PaperLedger` directly.

## Paper Maker PnL

Maker PnL is paper-only. The accounting path is:

```text
ApprovedQuote
  -> PaperMakerExecutionAdapter
  -> MakerExecutionReport
  -> PaperEventAdapter
  -> PaperLedger
  -> MakerPnLEngine
  -> verify_market_making_pnl_workflow
```

Only filled maker execution reports produce `PaperFill` records. `QuoteIntent`,
`ApprovedQuote`, and `QuoteRiskDecision` are observation-only and must not create
fills or PnL.

The ledger applies maker fills idempotently:

- maker bid fills are BUY fills that increase long inventory and reduce cash
- maker ask fills are SELL fills that reduce long inventory and realize PnL
- duplicate maker reports/fills are ignored
- maker sells that exceed current long inventory are rejected

`NoFill` mode stores approved quotes but produces zero maker fills and zero maker
PnL. `Conservative` mode fills only on explicit trade-through evidence. A bid
fills when `trade_price_tick <= bid.price_tick`; an ask fills when
`trade_price_tick >= ask.price_tick`.

`MakerPnLEngine` reports realized PnL, mid-mark unrealized PnL, and liquidation
PnL separately. Mid PnL uses the bid/ask midpoint when both sides are present.
Liquidation PnL uses the best bid for long inventory; if the bid is missing the
liquidation mark is zero and the mark quality is `MissingBid`. Stale, crossed,
recovering, closed, resolved, or otherwise unusable books are marked degraded
and are not reported as `Good`.

Adverse selection is horizon-based. Records remain pending until a mark exists at
or beyond the configured horizon; missing marks are not forced to zero.

Verification:

```bash
build/verify_market_making_pnl_workflow \
  --approved-quotes tests/fixtures/market_making/approved_quotes_roundtrip.jsonl \
  --market-events tests/fixtures/market_making/maker_trade_through_events.jsonl \
  --snapshots tests/fixtures/market_making/mark_snapshots.jsonl \
  --starting-cash 100000000000 \
  --fill-mode conservative \
  --check-determinism
```

No live execution, API, signer, wallet, network path, frontend, or dashboard API
is added by this paper PnL workflow.

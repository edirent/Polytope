# Market State Live-like Validation Report

Status: accepted with live-data caveat

This report covers the Step 11 live-like validation paths:

```text
1. run_market_state_live_smoke
2. chain_ws_http_parity_smoke
3. multi_asset_synthetic_workflow
```

## Important Input Note

The supplied token id:

```text
113331598718835447619835372415650100713271301516293755503999990681415131593110
```

did not currently have a CLOB order book. A direct CLOB book check returned:

```text
No orderbook exists for the requested token id
```

A 300 second live run with that token connected successfully and maintained
heartbeat, but produced no book snapshot/delta:

```text
ws_packets: 45
normalized_events: 44
book_snapshots: 0
book_deltas: 0
snapshots_published: 0
decode_errors: 0
transport_errors: 0
ping_sent: 30
pong_received: 30
```

That run is a correct negative result, not a MarketState acceptance run.

The accepted live smoke used an active token confirmed to have order book depth:

```text
market_id: 0x1fad72fae204143ff1c3035e99e7c0f65ea8d5cd9bd1070987bd1a3316f772be
asset_id: 98022490269692409998126496127597032490334070080325855126491859374983463996227
```

## 1. run_market_state_live_smoke

Path:

```text
Polymarket WS
  -> DecodePipeline
  -> MarketStateStore
  -> MarketStateView

Polygon WS logs
  -> OrderFilledDecoder
  -> ClassifiedFill
  -> MarketStateStore
  -> MarketStateView
```

Accepted command:

```text
build/run_market_state_live_smoke \
  --seconds 300 \
  --asset-id 98022490269692409998126496127597032490334070080325855126491859374983463996227 \
  --market-id 0x1fad72fae204143ff1c3035e99e7c0f65ea8d5cd9bd1070987bd1a3316f772be
```

Result:

```text
market_state_live_smoke:
  runtime_seconds: 300
  ws_packets: 954
  normalized_events: 954
  filtered_events: 225
  book_snapshots: 1
  book_deltas: 697
  chain_logs: 0
  classified_fills: 0
  buy_aggressor_fills: 0
  sell_aggressor_fills: 0
  unknown_fills: 0
  removed_fills: 0
  snapshots_published: 954
  usable_for_depth_count: 954
  usable_for_signal_count: 0
  state_errors: 0
  decode_errors: 0
  chain_decode_errors: 0
  transport_errors: 0
  ping_sent: 30
  pong_received: 30
```

Assessment:

```text
accepted: true
WS book path produced snapshots: true
MarketStateSnapshot published continuously: true
usable_for_depth observed: true
decode_errors: 0
state_errors: 0
transport_errors: 0
heartbeat stable: true
```

`filtered_events` are paired outcome asset events present in real Polymarket
`price_change` payloads. The live smoke keeps the validation scoped to the
requested asset so paired-asset deltas do not create false delta-before-snapshot
state errors for the target market state.

No chain fills for this asset were observed in this 300 second window, so this
run validates chain-stream liveness wiring but not live fill classification
coverage for the selected asset.

## 2. chain_ws_http_parity_smoke

Path:

```text
Polygon WS eth_subscribe logs
  vs
Polygon HTTP eth_getLogs over the same block range
```

ABI note:

```text
The live CTF Exchange address is Polymarket CTF Exchange V2.

OrderFilled topic0:
0xd543adfd945773f1a62f74f0ee55a5e3b9b1a28262980ba90b1a89f2ea84d8ee

V2 indexed topics:
  topic1: orderHash
  topic2: maker
  topic3: taker

V2 data fields:
  side
  tokenId
  makerAmountFilled
  takerAmountFilled
  fee
  builder
  metadata
```

This is different from the older conditionId/tokenId-indexed shape. For CTF
Exchange V2, tokenId is not an indexed topic, so token filtering is done after
HTTP/WS log retrieval by decoding the event data. Generic `--topic1`,
`--topic2`, and `--topic3` filters are still available for indexed-topic
experiments.

Contract filter:

```text
Polymarket CTF Exchange:
0xE111180000d2663C0091e4f400237545B87B996B
```

Accepted command:

```text
build/chain_ws_http_parity_smoke \
  --seconds 30 \
  --contract-address 0xE111180000d2663C0091e4f400237545B87B996B
```

Implementation note:

```text
HTTP eth_getLogs backfill is chunked into 10-block ranges.
```

Reason: the tested Alchemy free-tier endpoint rejects wider `eth_getLogs`
ranges. The smoke originally failed with a JSON-RPC error until the HTTP
backfill path was changed to chunked requests.

Result:

```text
chain_ws_http_parity_smoke:
  mode: live_parity
  start_block: 87554893
  end_block: 87554909
  ws_logs_seen: 1611
  http_logs_backfilled: 1611
  ws_logs_filtered_out: 0
  http_logs_filtered_out: 0
  decoded_order_fills: 3326
  missing_from_ws: 0
  extra_in_ws: 0
  duplicates: 0
  decode_errors: 0
  removed_logs: 0
  subscription_opened: true
  http_ok: true
```

Assessment:

```text
accepted: true
subscription_opened: true
http_backfill_ok: true
ws_http_counts_equal: true
missing_from_ws: 0
extra_in_ws: 0
duplicates: 0
decode_errors: 0
```

The live parity comparator intentionally starts at the first full block after
subscription setup and ignores the most recent two blocks at shutdown. This
avoids counting subscription/open and shutdown boundary logs as false
WS/HTTP mismatches.

## 2B. Historical Non-empty Chain Backfill

Purpose:

```text
Avoid blind live-window validation.
Force the chain confirmation pipeline through a known historical block that
contains real CTF Exchange V2 OrderFilled logs.
```

Accepted command:

```text
build/chain_ws_http_parity_smoke \
  --seconds 1 \
  --start-block 86408470 \
  --end-block 86408470 \
  --require-logs
```

Result:

```text
chain_ws_http_parity_smoke:
  mode: historical_backfill
  start_block: 86408470
  end_block: 86408470
  ws_logs_seen: 0
  http_logs_backfilled: 141
  ws_logs_filtered_out: 0
  http_logs_filtered_out: 0
  decoded_order_fills: 141
  missing_from_ws: 0
  extra_in_ws: 0
  duplicates: 0
  decode_errors: 0
  removed_logs: 0
  subscription_opened: false
  http_ok: true
```

Token-filtered command:

```text
build/chain_ws_http_parity_smoke \
  --seconds 1 \
  --start-block 86408470 \
  --end-block 86408470 \
  --token-id 15633047470204997596942995214398956213394532924233275212304274396386988607753 \
  --require-logs
```

Token-filtered result:

```text
chain_ws_http_parity_smoke:
  mode: historical_backfill
  start_block: 86408470
  end_block: 86408470
  ws_logs_seen: 0
  http_logs_backfilled: 5
  ws_logs_filtered_out: 0
  http_logs_filtered_out: 136
  decoded_order_fills: 141
  missing_from_ws: 0
  extra_in_ws: 0
  duplicates: 0
  decode_errors: 0
  removed_logs: 0
  subscription_opened: false
  http_ok: true
```

Assessment:

```text
accepted: true
historical non-empty backfill works: true
real OrderFilled logs decoded: 141
tokenId post-filter works: true
decode_errors: 0
```

## 3. multi_asset_synthetic_workflow

Path:

```text
asset A:
  book + confirmed buy/sell fills

asset B:
  book + ambiguous fill

asset C:
  delta before snapshot
```

Command:

```text
build/multi_asset_synthetic_workflow
```

Accepted output:

```text
multi_asset_synthetic_workflow:
asset_a:
  snapshot_ok: true
  chain_state_ok: true
  book_hash_unchanged: true
  confirmed_buy_lots_10s: 1000
  confirmed_sell_lots_10s: 700
asset_b:
  snapshot_ok: true
  ambiguous_not_counted: true
  confirmed_buy_lots_10s: 0
  confirmed_sell_lots_10s: 0
asset_c:
  snapshot_ok: true
  recovering: true
isolation:
  asset_isolation_ok: true
  asset_a_hash: 4814675256714439227
  asset_b_hash: 5649489820651407338
  asset_c_hash: 8771988790030926624
  global_book_hash: 17693413204777321940
```

Assessment:

```text
accepted: true
asset A snapshot ok: true
asset A chain state ok: true
asset A book hash unchanged after chain fills: true
asset B ambiguous not counted as buy/sell: true
asset C recovering after delta-before-snapshot: true
asset state isolation: true
```

## Test Suite

Command:

```text
ctest --test-dir build --output-on-failure
```

Result:

```text
100% tests passed
0 tests failed
191 tests passed
3 manual tests disabled
```

Disabled manual tests:

```text
MarketStateLiveSmokeManual
ChainWsHttpParitySmokeManual
LiveFeedSmokeManual
```

Offline baseline regression:

```text
build/run_feed_e2e tests/fixtures/polymarket/market_39.raw \
  --repeat 10000 \
  --check-determinism

global_hash: 12959912045291989833
trace_equal: true
determinism passed: true
```

## Final Assessment

Accepted:

```text
run_market_state_live_smoke with active order-book token
chain_ws_http_parity_smoke over a live Polygon block range
historical non-empty OrderFilled backfill and tokenId filtering
multi_asset_synthetic_workflow
default ctest suite
```

Still not proven:

```text
long-running WS/HTTP parity over many hours
multi-market live contamination behavior
strategy usefulness of the resulting state
```

Conclusion:

```text
Market State live-like validation v1 is accepted for:
  WS book -> Decode -> State -> Snapshot
  Polygon WS/HTTP parity plumbing
  synthetic multi-asset state isolation

It should not yet be treated as proof of live chain fill coverage because the
accepted live parity smoke is still a short window. Historical non-empty
backfill proves the decoder/backfill path on real chain logs, and the short
live parity run proves strict WS/HTTP equality for the sampled block range.
```

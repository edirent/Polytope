# Polymarket Event Research

This folder contains read-only research tooling for screening Polymarket events
for market-making suitability. It is intentionally outside the hot trading path.

The workflow:

1. Fetch active Gamma markets.
2. Keep markets with order books and reward metadata.
3. Aggregate markets into events.
4. Score event-level adverse-selection risk from transparent features.
5. Optionally call the existing OpenRouter environment contract used by
   `oracle/llm/OpenRouterRuleExtractor`:
   - `OPENROUTER_API_KEY`
   - `OPENROUTER_MODEL`
   - `OPENROUTER_MAX_TOKENS`
6. Write CSV, JSON, and Markdown reports.

Example:

```bash
python3 research/polymarket_event_research.py \
  --limit 1000 \
  --top-events 25 \
  --use-llm
```

If `OPENROUTER_API_KEY` is not set, the script records the LLM status as
`missing_api_key` and falls back to deterministic heuristics.

## Fair Value Pipeline

`fair_value_pipeline.py` standardizes external fair-value comparisons against
Polymarket prices.

Supported research methods:

- Crypto binary markets:
  `Fair Value = N(d2)` for cash-or-nothing calls.
- Sports/event markets:
  multiplicative de-vig from decimal odds.
- Signal metrics:
  absolute spread, z-score from historical spreads, expected value, and side.

Example:

```bash
python3 research/fair_value_pipeline.py \
  --config research/examples/fair_value_sample.json \
  --spread-history-csv research/examples/spread_history_sample.csv
```

Notes:

- Binance and Polymarket CLOB public endpoints can be region/CDN blocked. The
  script records source status and supports explicit config values and Gamma
  fallback prices.
- Pinnacle/Betfair require authenticated access, so sports odds are accepted as
  CSV/config inputs rather than connecting to private APIs here.
- The output is research-only and is not an order, risk approval, or execution
  decision.

## WS Data Pipeline

`ws_data_pipeline.py` is the research capture layer for the full external data
matrix. It is WebSocket-first and records every message with:

- local receive timestamp: `recv_ts_ns`
- parsed exchange timestamp when available: `exchange_ts_ns`
- computed source lag when possible: `lag_ns`
- original payload

Example:

```bash
python3 research/ws_data_pipeline.py \
  --config research/ws_research_config.example.json \
  --duration-seconds 30
```

Public sources currently wired:

- Deribit JSON-RPC WebSocket:
  - `ticker.{instrument_name}.raw`
  - `public/get_historical_volatility` over WS JSON-RPC
  - `public/get_order_book` can be added to the `requests` array
- Coinbase Advanced Trade WebSocket:
  - endpoint: `wss://advanced-trade-ws.coinbase.com`
  - public `ticker`, `level2`, and `heartbeats` channels
  - preferred US-node spot source for `S` in crypto binary pricing
- Kraken WebSocket v2:
  - endpoint: `wss://ws.kraken.com/v2`
  - `ticker` and `book` channels
  - redundant US-friendly spot source
- Binance WebSocket:
  - optional spot ticker streams such as `btcusdt@ticker`
  - futures depth streams such as `btcusdt@depth5@100ms`
  - on some US-hosted nodes, Binance spot may return regional restriction
    errors; Coinbase/Kraken are the primary spot fallback path
- Polymarket market WebSocket:
  - official endpoint:
    `wss://ws-subscriptions-clob.polymarket.com/ws/market`
  - subscribes by `assets_ids`
  - captures `book`, `price_change`, `last_trade_price`, and best bid/ask
    updates when enabled by the source

Private/licensed sources are adapter placeholders until credentials are
provided:

- Pinnacle: `PINNACLE_API_KEY`
- Betfair: `BETFAIR_APP_KEY`
- Sportsradar: `SPORTSRADAR_API_KEY`
- Opta: `OPTA_API_KEY`

The collector does not fake missing private data. It writes a diagnostic event
instead.

## 7-14 Day Research Deliverable

After collecting enough data, the research report should produce a whitelist
matrix with:

- branch / instrument
- external fair-value source
- average observed spread
- spread half-life
- best market-maker action
- defensive risk trigger

Example rows:

| Branch | Fair Source | Avg Spread | Half-Life | Action | Defense |
|---|---|---:|---:|---|---|
| BTC weekly binary | Binance spot + Deribit IV | 0.015 | 350 ms | passive two-sided making + rewards | cancel if 1m spot move > 1.5% |
| NBA moneyline | Betfair/Pinnacle de-vig | 0.008 | 1200 ms | aggressive taker on extreme deviation | pause on score event |
| Fed cuts multi-branch | CME/FedWatch proxy | 0.022 | 2500 ms | dynamic hedge / bundle quoting | cancel before CPI/FOMC |

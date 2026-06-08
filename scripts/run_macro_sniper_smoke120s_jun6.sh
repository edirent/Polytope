#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="runs/market_maker_btc_above_64k_jun6_macro_sniper_smoke120s_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RUN_DIR"
echo "$RUN_DIR" > /tmp/polytope_last_smoke120s_run_dir
: > "$RUN_DIR/live_report.jsonl"

./build/run_market_maker_dashboard_live \
  --seconds 120 \
  --asset-id 112398817528321414905784177304301034258491728234850452012159631147707415480339 \
  --complement-asset-id 89505570798936640022510973315593494452774010007599343836930564230876682470974 \
  --market-id btc-above-64000-on-june-6 \
  --dashboard-file "$RUN_DIR/dashboard.json" \
  --out-json "$RUN_DIR/summary.json" \
  --window-end-unix-seconds 1780761600 \
  --starting-cash 1000 \
  --initial-position-lots 25 \
  --initial-position-price 0.184 \
  --seed-complete-set \
  --target-position-lots 25 \
  --min-inventory-lots 0 \
  --max-inventory-lots 50 \
  --quote-size-lots 5 \
  --max-fill-qty-per-trade 10 \
  --pure-taker-mode \
  --external-fair-weight-bps 10000 \
  --require-external-fair \
  --enable-binance-btc-oracle \
  --btc-threshold 64000 \
  --btc-vol-annual-bps 1800 \
  --btc-oracle-max-age-ms 2000 \
  --assumed-latency-ms 2 \
  --latency-buffer-tick-per-ms 250 \
  --adverse-selection-buffer-tick 1000 \
  --disable-lead-lag-sniping \
  --enable-macro-divergence-taker \
  --macro-divergence-ewma-alpha 0.01 \
  --macro-divergence-min-edge-tick 20000 \
  --macro-max-local-oracle-divergence-tick 50000 \
  --macro-divergence-cooldown-ms 60000 \
  --max-taker-fills-per-minute 10 \
  --macro-divergence-taker-size-lots 5 \
  --taker-max-entry-mid-slippage-tick 3000 \
  --locked-book-taker-min-edge-tick 20000 \
  --locked-book-taker-size-lots 5 \
  --book-quarantine-ms 75 \
  --min-book-spread-tick 1000 \
  --min-quote-price-change-tick 1000 \
  --passive-reduce-excess-lots 5 \
  --urgent-reduce-excess-lots 20 \
  --urgent-reduce-age-ms 30000 \
  --puke-reduce-age-ms 120000 \
  --urgent-reduce-pressure-bps 2500 \
  --puke-reduce-pressure-bps 7000 \
  --fill-mode book-cross > "$RUN_DIR/stdout.txt" 2>&1 &

RUN_PID=$!
echo "$RUN_PID" > "$RUN_DIR/pid"

for _ in $(seq 1 24); do
  if [[ -s "$RUN_DIR/dashboard.json" ]]; then
    jq -c --arg ts "$(date -Iseconds)" '{
      ts:$ts,
      runtime_seconds:.market_maker.runtime_seconds,
      taker_ioc_fills_applied:.market_maker.taker_ioc_fills_applied,
      macro_divergence_taker_ioc_fills_applied:.market_maker.macro_divergence_taker_ioc_fills_applied,
      latest_reduce_exit_stage:.market_maker.latest_reduce_exit_stage,
      latest_reduce_exit_age_ms:.market_maker.latest_reduce_exit_age_ms,
      latest_reduce_exit_excess_lots:.market_maker.latest_reduce_exit_excess_lots,
      reduce_exit_quotes_submitted:.market_maker.reduce_exit_quotes_submitted,
      macro_edge_not_crossing_blocked:.market_maker.macro_edge_not_crossing_blocked,
      macro_structural_dislocation_blocked:.market_maker.macro_structural_dislocation_blocked
    }' "$RUN_DIR/dashboard.json" >> "$RUN_DIR/live_report.jsonl" 2>/dev/null || true
  fi
  sleep 5
done

wait "$RUN_PID" || true
STATUS=$?
printf '{"type":"run_exit","ts":"%s","status":%d}\n' "$(date -Iseconds)" "$STATUS" >> "$RUN_DIR/live_report.jsonl"
echo "DONE status=$STATUS RUN_DIR=$RUN_DIR"

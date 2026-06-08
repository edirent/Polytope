#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="runs/market_maker_btc_above_64k_jun6_macro_sniper_live30m_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RUN_DIR"
echo "$RUN_DIR" > /tmp/polytope_last_live30m_run_dir
: > "$RUN_DIR/live_report.jsonl"

./build/run_market_maker_dashboard_live \
  --seconds 1800 \
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
  --basis-ewma-alpha 0.001 \
  --macro-shock-min-edge-tick 20000 \
  --max-allowed-spread-tick 20000 \
  --max-allowed-basis-tick 300000 \
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
echo "LIVE30M_RUN_DIR=$RUN_DIR"
echo "LIVE30M_PID=$RUN_PID"

while kill -0 "$RUN_PID" 2>/dev/null; do
  if [[ -s "$RUN_DIR/dashboard.json" ]]; then
    jq -c --arg ts "$(date -Iseconds)" '{
      type:"snapshot",
      ts:$ts,
      runtime_seconds:.market_maker.runtime_seconds,
      btc_oracle_spot:.market_maker.btc_oracle_spot,
      btc_oracle_age_ms:.market_maker.btc_oracle_age_ms,
      latest_fair_value_tick:.market_maker.latest_fair_value_tick,
      external_fair_value_tick:.market_maker.external_fair_value_tick,
      latest_ewma_fair_yes_tick:.market_maker.latest_ewma_fair_yes_tick,
      latest_ewma_basis_yes_tick:.market_maker.latest_ewma_basis_yes_tick,
      yes_bid_tick:.market_maker.best_bid_tick,
      yes_ask_tick:.market_maker.best_ask_tick,
      equity_mid_tick:.market_maker.equity_mid_tick,
      seed_total_pnl_mid_tick:.market_maker.seed_total_pnl_mid_tick,
      strategy_total_pnl_mid_tick:.market_maker.strategy_total_pnl_mid_tick,
      strategy_realized_pnl_tick:.market_maker.strategy_realized_pnl_tick,
      strategy_unrealized_pnl_mid_tick:.market_maker.strategy_unrealized_pnl_mid_tick,
      strategy_complement_unrealized_pnl_mid_tick:.market_maker.strategy_complement_unrealized_pnl_mid_tick,
      strategy_spread_capture_tick:.market_maker.strategy_spread_capture_tick,
      yes_position_lots:.market_maker.open_position_lots,
      no_position_lots:.market_maker.complement_position_lots,
      condition_net_exposure_lots:.market_maker.condition_net_exposure_lots,
      macro_divergence_sniping_signals:.market_maker.macro_divergence_sniping_signals,
      macro_divergence_taker_ioc_fills_applied:.market_maker.macro_divergence_taker_ioc_fills_applied,
      macro_basis_uninitialized_blocked:.market_maker.macro_basis_uninitialized_blocked,
      macro_basis_insanity_blocked:.market_maker.macro_basis_insanity_blocked,
      macro_edge_not_crossing_blocked:.market_maker.macro_edge_not_crossing_blocked,
      taker_circuit_breaker_tripped:.market_maker.taker_circuit_breaker_tripped,
      latest_lead_lag_side:.market_maker.latest_lead_lag_side,
      taker_ioc_fills_applied:.market_maker.taker_ioc_fills_applied,
      taker_ioc_yes_fills_applied:.market_maker.taker_ioc_yes_fills_applied,
      taker_ioc_no_fills_applied:.market_maker.taker_ioc_no_fills_applied,
      taker_ioc_mid_slippage_blocked:.market_maker.taker_ioc_mid_slippage_blocked,
      taker_ioc_inventory_blocked:.market_maker.taker_ioc_inventory_blocked,
      taker_ioc_cooldown_blocked:.market_maker.taker_ioc_cooldown_blocked,
      maker_fills_applied:.market_maker.maker_fills_applied,
      submitted_quotes:.market_maker.submitted_quotes,
      cancelled_quotes:.market_maker.cancelled_quotes,
      reduce_exit_quotes_submitted:.market_maker.reduce_exit_quotes_submitted,
      reduce_exit_passive_quotes:.market_maker.reduce_exit_passive_quotes,
      reduce_exit_urgent_quotes:.market_maker.reduce_exit_urgent_quotes,
      reduce_exit_puke_quotes:.market_maker.reduce_exit_puke_quotes,
      latest_reduce_exit_asset_side:.market_maker.latest_reduce_exit_asset_side,
      latest_reduce_exit_stage:.market_maker.latest_reduce_exit_stage,
      latest_reduce_exit_age_ms:.market_maker.latest_reduce_exit_age_ms,
      latest_reduce_exit_excess_lots:.market_maker.latest_reduce_exit_excess_lots,
      latest_reduce_exit_pressure_bps:.market_maker.latest_reduce_exit_pressure_bps,
      latest_reduce_exit_price_tick:.market_maker.latest_reduce_exit_price_tick,
      latest_reduce_exit_qty_lots:.market_maker.latest_reduce_exit_qty_lots,
      book_quarantine_events:.market_maker.book_quarantine_events,
      depth_updates_quarantined:.market_maker.depth_updates_quarantined,
      latest_latency_end_to_end_ns:.latency.end_to_end_ns,
      decode_errors:.market_maker.decode_errors,
      state_errors:.market_maker.state_errors,
      transport_errors:.market_maker.transport_errors,
      dashboard_write_errors:.market_maker.dashboard_write_errors
    }' "$RUN_DIR/dashboard.json" >> "$RUN_DIR/live_report.jsonl" 2>/dev/null || true
  fi
  sleep 60
done

wait "$RUN_PID"
STATUS=$?

if [[ -s "$RUN_DIR/summary.json" ]]; then
  jq -c --arg ts "$(date -Iseconds)" '{
    type:"final_summary",
    ts:$ts,
    runtime_seconds,
    equity_mid_tick,
    seed_total_pnl_mid_tick,
    strategy_total_pnl_mid_tick,
    strategy_realized_pnl_tick,
    strategy_unrealized_pnl_mid_tick,
    strategy_complement_unrealized_pnl_mid_tick,
    strategy_spread_capture_tick,
    open_position_lots,
    complement_position_lots,
    condition_net_exposure_lots,
    macro_divergence_sniping_signals,
    macro_divergence_taker_ioc_fills_applied,
    macro_structural_dislocation_blocked,
    macro_edge_not_crossing_blocked,
    taker_circuit_breaker_tripped,
    latest_lead_lag_side,
    taker_ioc_fills_applied,
    taker_ioc_yes_fills_applied,
    taker_ioc_no_fills_applied,
    taker_ioc_mid_slippage_blocked,
    taker_ioc_inventory_blocked,
    taker_ioc_cooldown_blocked,
    maker_fills_applied,
    submitted_quotes,
    cancelled_quotes,
    reduce_exit_quotes_submitted,
    reduce_exit_passive_quotes,
    reduce_exit_urgent_quotes,
    reduce_exit_puke_quotes,
    latest_reduce_exit_asset_side,
    latest_reduce_exit_stage,
    latest_reduce_exit_age_ms,
    latest_reduce_exit_excess_lots,
    latest_reduce_exit_pressure_bps,
    latest_reduce_exit_price_tick,
    latest_reduce_exit_qty_lots,
    book_quarantine_events,
    depth_updates_quarantined,
    btc_oracle_updates,
    decode_errors,
    state_errors,
    transport_errors,
    dashboard_write_errors,
    pipeline_latency_ns
  }' "$RUN_DIR/summary.json" >> "$RUN_DIR/live_report.jsonl" 2>/dev/null || true
fi

printf '{"type":"run_exit","ts":"%s","status":%d}\n' "$(date -Iseconds)" "$STATUS" >> "$RUN_DIR/live_report.jsonl"
exit "$STATUS"

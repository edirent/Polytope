#!/usr/bin/env bash
set -euo pipefail

TMP_DIR="/tmp/external_fair_basis_research_test"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"

cat > "$TMP_DIR/external_fair_basis_snapshots.csv" <<'CSV'
ts_ms,market_id,token_id,symbol,event_type,barrier_price,outcome_side,spot,annualized_vol,tte_years,external_fair_tick,yes_probability,best_bid_tick,best_ask_tick,bid_size,ask_size,book_mid_tick,book_micro_tick,spread_tick,mid_basis_tick,micro_basis_tick,buy_edge_tick,sell_edge_tick,book_age_ms,spot_age_ms,vol_age_ms
1000,sol_above90,tok1,SOL,UpTouch,90,Yes,72,0.9,0.06,300000,0.30,280000,290000,100,100,285000,285000,10000,-15000,-15000,10000,-20000,10,20,30
11000,sol_above90,tok1,SOL,UpTouch,90,Yes,72.5,0.9,0.06,310000,0.31,295000,305000,100,100,300000,300000,10000,-10000,-10000,5000,-15000,10,20,30
61000,sol_above90,tok1,SOL,UpTouch,90,Yes,73,0.9,0.06,330000,0.33,315000,325000,100,100,320000,320000,10000,-10000,-10000,5000,-15000,10,20,30
1000,sol_below60,tok2,SOL,DownTouch,60,Yes,72,0.9,0.06,400000,0.40,410000,420000,100,100,415000,415000,10000,15000,15000,-20000,10000,10,20,30
11000,sol_below60,tok2,SOL,DownTouch,60,Yes,71.5,0.9,0.06,420000,0.42,405000,415000,100,100,410000,410000,10000,-10000,-10000,5000,-15000,10,20,30
61000,sol_below60,tok2,SOL,DownTouch,60,Yes,71,0.9,0.06,430000,0.43,400000,410000,100,100,405000,405000,10000,-25000,-25000,20000,-30000,10,20,30
CSV

python3 tools/analyze_external_fair_basis.py \
  --input "$TMP_DIR/external_fair_basis_snapshots.csv" \
  --output-dir "$TMP_DIR/out" \
  --markout-horizons-seconds 10,60 \
  --price-scale-tick 1000000 \
  --max-spread-cents 0.5 \
  --edge-threshold-cents 0.5,1.0,2.0

test -f "$TMP_DIR/out/external_fair_basis_enriched.csv"
test -f "$TMP_DIR/out/basis_summary.csv"
test -f "$TMP_DIR/out/edge_frequency.csv"
test -f "$TMP_DIR/out/markout_by_basis_bucket.csv"
test -f "$TMP_DIR/out/markout_by_spread_bucket.csv"
test -f "$TMP_DIR/out/markout_by_z_bucket.csv"
test -f "$TMP_DIR/out/trade_signal_report.csv"

grep -q "market_mid_markout_tick_10s" "$TMP_DIR/out/external_fair_basis_enriched.csv"
grep -q "external_fair_markout_tick_10s" "$TMP_DIR/out/external_fair_basis_enriched.csv"
grep -q "basis_change_tick_10s" "$TMP_DIR/out/external_fair_basis_enriched.csv"
grep -q "buy_edge_0p5c" "$TMP_DIR/out/trade_signal_report.csv"

echo "external fair basis research verification passed"

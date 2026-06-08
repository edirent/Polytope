#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_ROOT="runs/sol_paper_mm_60_90_20m_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RUN_ROOT"
echo "$RUN_ROOT" > /tmp/polytope_last_sol_paper_mm_run_dir

# What price will Solana hit in June? — SOL above $60 / $90 (June 8 expiry)
# Token IDs from runs/edge_discovery/latest_14d/markets_universe.csv
SOL60_YES="23020209449371201556640889448557289524435108691406679039120795527093517792816"
SOL60_NO="65315774247466926456458486063890657042341457893475440310458530598798567028717"
SOL90_YES="25689518860472437603223239700548809121375077889675579554279849895913155347939"
SOL90_NO="15465189016156313669386875027454548874822330291499031265975230159724281720078"
WINDOW_END_UNIX_SECONDS=1780934400

COMMON_ARGS=(
  --seconds 1200
  --starting-cash 5000
  --window-end-unix-seconds "$WINDOW_END_UNIX_SECONDS"
  --target-position-lots 0
  --min-inventory-lots 0
  --max-inventory-lots 50
  --quote-size-lots 5
  --half-spread-tick 5000
  --max-fill-qty-per-trade 5
  --min-fair-confidence-bps 0
  --max-book-spread-bps 15000
  --book-quarantine-ms 0
  --allow-book-fair-opening
  --disable-macro-divergence-taker
  --disable-lead-lag-sniping
  --disable-locked-book-taker-hunter
  --fill-mode book-cross
)

run_market() {
  local label="$1"
  local asset_id="$2"
  local complement_asset_id="$3"
  local market_id="$4"
  local run_dir="$RUN_ROOT/$label"
  mkdir -p "$run_dir"
  : > "$run_dir/live_report.jsonl"

  ./build/run_market_maker_dashboard_live \
    "${COMMON_ARGS[@]}" \
    --asset-id "$asset_id" \
    --complement-asset-id "$complement_asset_id" \
    --market-id "$market_id" \
    --dashboard-file "$run_dir/dashboard.json" \
    --out-json "$run_dir/summary.json" \
    > "$run_dir/stdout.txt" 2>&1 &

  local pid=$!
  echo "$pid" > "$run_dir/pid"
  echo "started $label pid=$pid dir=$run_dir"
}

run_market sol60 "$SOL60_YES" "$SOL60_NO" "solana-above-60-on-june-8-2026"
run_market sol90 "$SOL90_YES" "$SOL90_NO" "solana-above-90-on-june-8-2026"

echo "RUN_ROOT=$RUN_ROOT"
echo "Duration: 20 minutes (1200s)"
echo "Starting cash per market: \$5000 paper"

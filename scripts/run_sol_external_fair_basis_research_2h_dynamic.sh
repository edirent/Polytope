#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_ROOT="runs/sol_external_fair_basis_research_2h_dynamic_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RUN_ROOT"
echo "$RUN_ROOT" > /tmp/polytope_last_sol_external_fair_basis_run_dir

DURATION_SECONDS=7200
SOL_VOL=0.90

# Keep token IDs and market expiry in one place by reusing the existing 20m script.
SOURCE_SCRIPT="scripts/run_sol_external_fair_basis_research_20m.sh"
if [[ ! -f "$SOURCE_SCRIPT" ]]; then
  echo "missing source script: $SOURCE_SCRIPT" >&2
  exit 1
fi
eval "$(
  sed -n -E \
    -e '/^(SOL90_YES|SOL90_NO|SOL60_YES|SOL60_NO|SOL50_YES|SOL50_NO|WINDOW_END_UNIX_SECONDS)=/p' \
    "$SOURCE_SCRIPT"
)"

COMMON_ARGS=(
  --sol-spot-feed binance_book_ticker
  --sol-fixed-vol-annualized "$SOL_VOL"
  --external-fair-basis-log
  --external-fair-weight-bps 0
  --duration-seconds "$DURATION_SECONDS"
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
  --min-book-spread-tick 2000
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
  local outcome_side="$5"

  local run_dir="$RUN_ROOT/$label"
  mkdir -p "$run_dir"
  : > "$run_dir/live_report.jsonl"

  ./build/run_market_maker_dashboard_live \
    "${COMMON_ARGS[@]}" \
    --sol-external-fair-outcome-side "$outcome_side" \
    --asset-id "$asset_id" \
    --complement-asset-id "$complement_asset_id" \
    --market-id "$market_id" \
    --dashboard-file "$run_dir/dashboard.json" \
    --out-json "$run_dir/summary.json" \
    > "$run_dir/stdout.txt" 2>&1 &

  local pid=$!
  echo "$pid" > "$run_dir/pid"
  echo "started $label pid=$pid dir=$run_dir outcome=$outcome_side"
}

run_market sol_june_above90 "$SOL90_YES" "$SOL90_NO" \
  "will-solana-reach-90-in-june-2026" yes
run_market sol_june_below60 "$SOL60_NO" "$SOL60_YES" \
  "will-solana-dip-to-60-in-june-2026" no
run_market sol_june_below50 "$SOL50_NO" "$SOL50_YES" \
  "will-solana-dip-to-50-in-june-2026" no

echo "RUN_ROOT=$RUN_ROOT"
echo "SOL spot feed: binance_book_ticker"
echo "SOL fixed vol: $SOL_VOL"
echo "Duration: 2 hours (${DURATION_SECONDS}s)"
echo "Basis snapshots: <run_dir>/<label>/external_fair_basis_snapshots.csv"

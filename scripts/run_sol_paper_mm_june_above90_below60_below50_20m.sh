#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_ROOT="runs/sol_paper_mm_june_above90_below60_below50_20m_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RUN_ROOT"
echo "$RUN_ROOT" > /tmp/polytope_last_sol_paper_mm_run_dir

# What price will Solana hit in June? (2026 monthly event)
# Event slug: what-price-will-solana-hit-in-june-2026
# Source: https://gamma-api.polymarket.com/events/slug/what-price-will-solana-hit-in-june-2026

# SOL reach $90 in June (upside strike)
SOL90_YES="61187734443395207848047264922559788359121248004877755109606319689344271875085"
SOL90_NO="9836515992090559553668479129748900118639534423907920910936547117900310080633"

# SOL dip to $60 in June (downside $60 strike; monthly has no separate "reach $60" market)
SOL60_YES="85687970084307950280029939700668253105869588558732470599246318222221223515521"
SOL60_NO="107442861600849437983944503218238678148719380594313374464058990586364215617885"

# SOL dip to $50 in June (downside $50 strike)
SOL50_YES="22522885117208693592916412108701823878236389773421022112592011955044817646045"
SOL50_NO="30862275651098159474561814320959814330872562492287735220495872617124948398440"

# Monthly June expiry: 2026-07-01 04:00:00 UTC (Gamma endDate)
WINDOW_END_UNIX_SECONDS=1782878400

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

# 1. Above $90 in June: quote YES on will-solana-reach-90-in-june-2026
run_market \
  sol_june_above90 \
  "$SOL90_YES" \
  "$SOL90_NO" \
  "will-solana-reach-90-in-june-2026"

# 2. Below $60 in June: quote NO on will-solana-dip-to-60-in-june-2026
#    (monthly downside $60 strike; NO = will not dip to $60)
run_market \
  sol_june_below60 \
  "$SOL60_NO" \
  "$SOL60_YES" \
  "will-solana-dip-to-60-in-june-2026"

# 3. Below $50 in June: quote NO on will-solana-dip-to-50-in-june-2026
run_market \
  sol_june_below50 \
  "$SOL50_NO" \
  "$SOL50_YES" \
  "will-solana-dip-to-50-in-june-2026"

echo "RUN_ROOT=$RUN_ROOT"
echo "Duration: 20 minutes (1200s)"
echo "Starting cash per market: \$5000 paper"
echo "Market family: What price will Solana hit in June? (2026 monthly)"
echo "Markets:"
echo "  - SOL reach \$90 in June: YES side (will-solana-reach-90-in-june-2026)"
echo "  - SOL dip to \$60 in June: NO side (will-solana-dip-to-60-in-june-2026)"
echo "  - SOL dip to \$50 in June: NO side (will-solana-dip-to-50-in-june-2026)"

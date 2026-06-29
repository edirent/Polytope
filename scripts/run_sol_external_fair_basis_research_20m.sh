#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Fetch SOL mid from CoinGecko (Binance may be geo-blocked).
SOL_MID="$(python3 - <<'PY'
import json
import urllib.request

req = urllib.request.Request(
    "https://api.coingecko.com/api/v3/simple/price?ids=solana&vs_currencies=usd",
    headers={"User-Agent": "polytope-basis-research/1.0"},
)
with urllib.request.urlopen(req, timeout=15) as resp:
    data = json.load(resp)
print(data["solana"]["usd"])
PY
)"
SOL_BID="$(python3 -c "print(f'{float('${SOL_MID}') - 0.01:.4f}')")"
SOL_ASK="$(python3 -c "print(f'{float('${SOL_MID}') + 0.01:.4f}')")"

RUN_ROOT="runs/sol_external_fair_basis_research_20m_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RUN_ROOT"
echo "$RUN_ROOT" > /tmp/polytope_last_sol_basis_research_run_dir

SOL90_YES="61187734443395207848047264922559788359121248004877755109606319689344271875085"
SOL90_NO="9836515992090559553668479129748900118639534423907920910936547117900310080633"
SOL60_YES="85687970084307950280029939700668253105869588558732470599246318222221223515521"
SOL60_NO="107442861600849437983944503218238678148719380594313374464058990586364215617885"
SOL50_YES="22522885117208693592916412108701823878236389773421022112592011955044817646045"
SOL50_NO="30862275651098159474561814320959814330872562492287735220495872617124948398440"
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
  --min-book-spread-tick 2000
  --book-quarantine-ms 0
  --allow-book-fair-opening
  --disable-macro-divergence-taker
  --disable-lead-lag-sniping
  --disable-locked-book-taker-hunter
  --fill-mode book-cross
  --external-fair-basis-log
  --sol-spot-bid "$SOL_BID"
  --sol-spot-ask "$SOL_ASK"
  --sol-fixed-vol-annualized 0.90
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
echo "SOL spot: bid=$SOL_BID ask=$SOL_ASK"
echo "Duration: 20 minutes (1200s)"
echo "Basis snapshots: <run_dir>/<label>/external_fair_basis_snapshots.csv"

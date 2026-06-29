#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="runs/sol_external_fair_basis_research_20m_20260608_052704"
MARKETS=(
  sol_june_above90
  sol_june_below60
  sol_june_below50
)

COMMON_ARGS=(
  --markout-horizons-seconds 10,30,60,300,900,1800
  --price-scale-tick 1000000
  --max-spread-cents 0.5
  --edge-threshold-cents 0.5,1.0,2.0
)

for market in "${MARKETS[@]}"; do
  input_csv="${RUN_DIR}/${market}/external_fair_basis_snapshots.csv"
  output_dir="${RUN_DIR}/${market}/basis_research_v2"

  if [[ ! -f "$input_csv" ]]; then
    echo "missing input CSV: $input_csv" >&2
    exit 1
  fi

  echo "reanalyzing ${market} -> ${output_dir}"
  python3 tools/analyze_external_fair_basis.py \
    --input "$input_csv" \
    --output-dir "$output_dir" \
    "${COMMON_ARGS[@]}"
done

echo "reanalysis complete for ${RUN_DIR}"

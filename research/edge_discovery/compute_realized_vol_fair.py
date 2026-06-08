#!/usr/bin/env python3
"""Compute realized-vol digital fair values for SOL usable buy rows."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

import pandas as pd


SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0
VOL_COLUMNS = {
    "5m": "realized_vol_5m",
    "1h": "realized_vol_1h",
    "24h": "realized_vol_24h",
}


def norm_cdf(value: float) -> float:
    return 0.5 * math.erfc(-value / math.sqrt(2.0))


def digital_call_probability(spot: Any, strike: Any, tau_years: Any, sigma: Any) -> float | None:
    try:
        s = float(spot)
        k = float(strike)
        tau = float(tau_years)
        vol = float(sigma)
    except (TypeError, ValueError):
        return None
    if not all(math.isfinite(x) for x in (s, k, tau, vol)):
        return None
    if s <= 0 or k <= 0 or vol <= 0:
        return None
    if tau <= 0:
        return 1.0 if s > k else 0.0
    sd = vol * math.sqrt(tau)
    if sd <= 0 or not math.isfinite(sd):
        return None
    d2 = (math.log(s / k) - 0.5 * vol * vol * tau) / sd
    return max(0.0, min(1.0, norm_cdf(d2)))


def read_inputs(poly_path: Path, external_path: Path) -> tuple[pd.DataFrame, pd.DataFrame]:
    poly = pd.read_parquet(poly_path)
    poly = poly[poly["coin"] == "SOL"].copy()
    if poly.empty:
        raise RuntimeError(f"no SOL rows in {poly_path}")
    external = pd.read_parquet(external_path).copy()
    external = external[external["coin"] == "SOL"].copy()
    if external.empty:
        raise RuntimeError(f"no SOL external rows in {external_path}")
    return poly.sort_values("timestamp"), external.sort_values("timestamp")


def compute_features(poly: pd.DataFrame, external: pd.DataFrame, tolerance_ms: int) -> pd.DataFrame:
    ext_cols = ["timestamp", "mid", "realized_vol_5m", "realized_vol_1h", "realized_vol_24h"]
    joined = pd.merge_asof(
        poly.sort_values("timestamp"),
        external[ext_cols].sort_values("timestamp").rename(columns={"timestamp": "external_ts"}),
        left_on="timestamp",
        right_on="external_ts",
        direction="backward",
        tolerance=tolerance_ms,
    )
    joined = joined.rename(columns={"mid": "spot_mid"})
    joined["external_staleness_ms"] = joined["timestamp"] - joined["external_ts"]
    joined["time_to_expiry_sec"] = joined["expiry_ts"] - (joined["timestamp"] / 1000.0)
    joined["tau_years"] = joined["time_to_expiry_sec"] / SECONDS_PER_YEAR

    for suffix, vol_col in VOL_COLUMNS.items():
        fair_col = f"fair_yes_rv_{suffix}"
        joined[fair_col] = [
            digital_call_probability(spot, strike, tau, sigma)
            for spot, strike, tau, sigma in zip(
                joined["spot_mid"],
                joined["strike"],
                joined["tau_years"],
                joined[vol_col],
            )
        ]
    joined["fair_yes_primary"] = joined["fair_yes_rv_1h"]
    joined["edge_buy_yes"] = joined["fair_yes_primary"] - joined["yes_exec_ask"]
    joined["is_candidate_3c"] = (
        (joined["edge_buy_yes"] >= 0.03)
        & (joined["yes_exec_ask"] >= 0.05)
        & (joined["yes_exec_ask"] <= 0.95)
        & (joined["time_to_expiry_sec"] > 600)
        & (joined["time_to_expiry_sec"] < 72 * 3600)
    )
    output_cols = [
        "timestamp",
        "datetime",
        "market_id",
        "slug",
        "coin",
        "strike",
        "expiry_ts",
        "spot_mid",
        "external_ts",
        "external_staleness_ms",
        "yes_exec_ask",
        "yes_exec_ask_source",
        "fair_yes_rv_5m",
        "fair_yes_rv_1h",
        "fair_yes_rv_24h",
        "fair_yes_primary",
        "edge_buy_yes",
        "time_to_expiry_sec",
        "realized_vol_5m",
        "realized_vol_1h",
        "realized_vol_24h",
        "is_candidate_3c",
    ]
    return joined[output_cols].copy()


def quality_report(features: pd.DataFrame, external: pd.DataFrame) -> dict[str, Any]:
    edge = pd.to_numeric(features["edge_buy_yes"], errors="coerce")
    return {
        "external_rows": int(len(external)),
        "external_first_ts": int(external["timestamp"].min()) if len(external) else None,
        "external_last_ts": int(external["timestamp"].max()) if len(external) else None,
        "external_coverage_hours": (float(external["timestamp"].max() - external["timestamp"].min()) / 3_600_000.0) if len(external) > 1 else 0.0,
        "realized_vol_1h_null_rate": float(external["realized_vol_1h"].isna().mean()) if len(external) else None,
        "joined_sol_fair_rows": int(len(features)),
        "joined_spot_null_rate": float(features["spot_mid"].isna().mean()) if len(features) else None,
        "fair_primary_null_rate": float(features["fair_yes_primary"].isna().mean()) if len(features) else None,
        "edge_ge_3c_count": int((edge >= 0.03).sum()),
        "edge_ge_5c_count": int((edge >= 0.05).sum()),
        "edge_ge_10c_count": int((edge >= 0.10).sum()),
        "candidate_3c_count": int(features["is_candidate_3c"].sum()),
        "by_market_edge_count": {
            str(market_id): int((group["edge_buy_yes"] >= 0.03).sum())
            for market_id, group in features.groupby("market_id")
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--poly-rows", default="runs/edge_discovery/latest_14d/poly_l2_sample_atm_filtered/usable_buy_rows.parquet")
    parser.add_argument("--external", default="runs/edge_discovery/latest_14d/external_crypto_5s/SOL.parquet")
    parser.add_argument("--out-dir", default="runs/edge_discovery/latest_14d/fair_value_v0")
    parser.add_argument("--join-tolerance-ms", type=int, default=10_000)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    poly, external = read_inputs(Path(args.poly_rows), Path(args.external))
    features = compute_features(poly, external, args.join_tolerance_ms)
    features.to_parquet(out_dir / "sol_buy_side_fair_features.parquet", index=False)
    report = quality_report(features, external)
    with (out_dir / "fair_quality.json").open("w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"wrote rows={len(features)} out_dir={out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

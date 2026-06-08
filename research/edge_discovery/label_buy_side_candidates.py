#!/usr/bin/env python3
"""Label SOL buy-side fair-value candidates with future ask/fair markouts."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

import pandas as pd


DEFAULT_FEATURES = "runs/edge_discovery/latest_14d/fair_value_v0/sol_buy_side_fair_features.parquet"
DEFAULT_USABLE_BUY = "runs/edge_discovery/latest_14d/poly_l2_sample_atm_filtered/usable_buy_rows.parquet"
DEFAULT_OUT_DIR = "runs/edge_discovery/latest_14d/fair_value_v0"
HORIZONS = {
    "60s": 60_000,
    "5m": 5 * 60_000,
    "30m": 30 * 60_000,
}


def load_resolution(path: str) -> dict[str, float]:
    if not path:
        return {}
    with open(path, encoding="utf-8") as handle:
        payload = json.load(handle)
    return {str(key): float(value) for key, value in payload.items()}


def select_candidates(features: pd.DataFrame) -> pd.DataFrame:
    source = features["yes_exec_ask_source"].fillna("").astype(str)
    return features[
        (features["coin"] == "SOL")
        & (features["edge_buy_yes"] >= 0.03)
        & (features["yes_exec_ask"] >= 0.05)
        & (features["yes_exec_ask"] <= 0.95)
        & (features["time_to_expiry_sec"] > 600)
        & (source != "")
        & (source != "missing")
    ].copy()


def attach_future_labels(candidates: pd.DataFrame, features: pd.DataFrame, tolerance_ms: int) -> pd.DataFrame:
    labeled_parts: list[pd.DataFrame] = []
    full = features.sort_values("timestamp")
    for market_id, cand_group in candidates.groupby("market_id", sort=False):
        market_series = full[full["market_id"] == market_id][
            ["timestamp", "yes_exec_ask", "fair_yes_primary"]
        ].sort_values("timestamp")
        if market_series.empty:
            labeled_parts.append(cand_group)
            continue
        labeled = cand_group.sort_values("timestamp").copy()
        for label, delta_ms in HORIZONS.items():
            target_col = f"target_ts_{label}"
            labeled[target_col] = labeled["timestamp"] + delta_ms
            future = market_series.rename(
                columns={
                    "timestamp": target_col,
                    "yes_exec_ask": f"future_yes_exec_ask_{label}",
                    "fair_yes_primary": f"future_fair_yes_{label}",
                }
            )
            labeled = pd.merge_asof(
                labeled.sort_values(target_col),
                future.sort_values(target_col),
                on=target_col,
                direction="forward",
                tolerance=tolerance_ms,
            ).sort_values("timestamp")
            labeled[f"ask_markout_{label}"] = labeled[f"future_yes_exec_ask_{label}"] - labeled["yes_exec_ask"]
            labeled[f"fair_change_{label}"] = labeled[f"future_fair_yes_{label}"] - labeled["fair_yes_primary"]
        labeled_parts.append(labeled)
    return pd.concat(labeled_parts, ignore_index=True) if labeled_parts else pd.DataFrame()


def attach_resolution(labeled: pd.DataFrame, resolution: dict[str, float]) -> pd.DataFrame:
    out = labeled.copy()
    out["resolution_yes"] = out["market_id"].map(resolution)
    out["resolved_pnl"] = out["resolution_yes"] - out["yes_exec_ask"]
    return out


def mean_median_win(series: pd.Series) -> dict[str, float | None]:
    values = pd.to_numeric(series, errors="coerce").dropna()
    if values.empty:
        return {"mean": None, "median": None, "win_rate": None}
    return {
        "mean": float(values.mean()),
        "median": float(values.median()),
        "win_rate": float((values > 0).mean()),
    }


def mean_median(series: pd.Series) -> dict[str, float | None]:
    values = pd.to_numeric(series, errors="coerce").dropna()
    if values.empty:
        return {"mean": None, "median": None}
    return {"mean": float(values.mean()), "median": float(values.median())}


def compact_rows(df: pd.DataFrame, limit: int = 20) -> list[dict[str, Any]]:
    cols = [
        "timestamp",
        "slug",
        "strike",
        "expiry_ts",
        "spot_mid",
        "yes_exec_ask",
        "fair_yes_rv_5m",
        "fair_yes_rv_1h",
        "fair_yes_rv_24h",
        "edge_buy_yes",
        "time_to_expiry_sec",
        "yes_exec_ask_source",
    ]
    rows: list[dict[str, Any]] = []
    for row in df.sort_values("edge_buy_yes", ascending=False).head(limit)[cols].to_dict("records"):
        rows.append({key: (None if isinstance(value, float) and math.isnan(value) else value) for key, value in row.items()})
    return rows


def dedup_5m(labeled: pd.DataFrame) -> pd.DataFrame:
    out = labeled.copy()
    out["bucket_5m"] = (out["timestamp"] // (5 * 60_000)) * (5 * 60_000)
    return (
        out.sort_values(["market_id", "bucket_5m", "edge_buy_yes"], ascending=[True, True, False])
        .drop_duplicates(["market_id", "bucket_5m"], keep="first")
        .copy()
    )


def build_report(labeled: pd.DataFrame, deduped: pd.DataFrame) -> dict[str, Any]:
    report: dict[str, Any] = {
        "candidate_rows": int(len(labeled)),
        "unique_markets": int(labeled["market_id"].nunique()) if len(labeled) else 0,
        "edge_ge_3c": int((labeled["edge_buy_yes"] >= 0.03).sum()) if len(labeled) else 0,
        "edge_ge_5c": int((labeled["edge_buy_yes"] >= 0.05).sum()) if len(labeled) else 0,
        "edge_ge_10c": int((labeled["edge_buy_yes"] >= 0.10).sum()) if len(labeled) else 0,
        "dedup_5m": {
            "candidate_rows": int(len(deduped)),
            "unique_markets": int(deduped["market_id"].nunique()) if len(deduped) else 0,
            "edge_ge_3c": int((deduped["edge_buy_yes"] >= 0.03).sum()) if len(deduped) else 0,
            "edge_ge_5c": int((deduped["edge_buy_yes"] >= 0.05).sum()) if len(deduped) else 0,
            "edge_ge_10c": int((deduped["edge_buy_yes"] >= 0.10).sum()) if len(deduped) else 0,
        },
        "ask_markout": {},
        "fair_change": {},
        "resolved_rows": int(labeled["resolved_pnl"].notna().sum()) if "resolved_pnl" in labeled else 0,
        "resolved_pnl_mean": None,
        "by_market_breakdown": {},
        "top_20_edge_rows": compact_rows(labeled),
    }
    for label in HORIZONS:
        report["ask_markout"][label] = mean_median_win(labeled[f"ask_markout_{label}"])
        report["fair_change"][label] = mean_median(labeled[f"fair_change_{label}"])
    if "resolved_pnl" in labeled and labeled["resolved_pnl"].notna().any():
        report["resolved_pnl_mean"] = float(labeled["resolved_pnl"].dropna().mean())
    for (market_id, slug), group in labeled.groupby(["market_id", "slug"], sort=False):
        report["by_market_breakdown"][str(market_id)] = {
            "slug": str(slug),
            "rows": int(len(group)),
            "edge_ge_3c": int((group["edge_buy_yes"] >= 0.03).sum()),
            "edge_ge_5c": int((group["edge_buy_yes"] >= 0.05).sum()),
            "edge_ge_10c": int((group["edge_buy_yes"] >= 0.10).sum()),
            "ask_markout_5m": mean_median_win(group["ask_markout_5m"]),
            "fair_change_5m": mean_median(group["fair_change_5m"]),
        }
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--features", default=DEFAULT_FEATURES)
    parser.add_argument("--usable-buy", default=DEFAULT_USABLE_BUY)
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    parser.add_argument("--future-tolerance-ms", type=int, default=10_000)
    parser.add_argument("--resolution-json", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    features = pd.read_parquet(args.features)
    # This read validates the expected upstream artifact exists. The feature file
    # already contains the usable buy rows with fair columns.
    usable_buy = pd.read_parquet(args.usable_buy)
    if usable_buy[usable_buy["coin"] == "SOL"].empty:
        raise RuntimeError(f"no SOL usable buy rows in {args.usable_buy}")
    candidates = select_candidates(features)
    labeled = attach_future_labels(candidates, features, args.future_tolerance_ms)
    labeled = attach_resolution(labeled, load_resolution(args.resolution_json))
    deduped = dedup_5m(labeled) if not labeled.empty else labeled.copy()
    labeled.to_parquet(out_dir / "sol_buy_side_labeled.parquet", index=False)
    report = build_report(labeled, deduped)
    with (out_dir / "label_report.json").open("w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"wrote candidates={len(labeled)} dedup_5m={len(deduped)} out_dir={out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

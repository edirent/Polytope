#!/usr/bin/env python3
"""Filter normalized PMXT L2 rows into usable buy/two-sided research sets."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import pandas as pd


DEFAULT_INPUT = "runs/edge_discovery/latest_14d/poly_l2_sample_atm_normalized/normalized_yes_space.parquet"
DEFAULT_OUT_DIR = "runs/edge_discovery/latest_14d/poly_l2_sample_atm_filtered"


def rate(mask: pd.Series) -> float:
    return float(mask.fillna(False).mean()) if len(mask) else 0.0


def percentile(series: pd.Series, q: float) -> float | None:
    values = pd.to_numeric(series, errors="coerce").dropna()
    if values.empty:
        return None
    return float(values.quantile(q))


def is_not_null(series: pd.Series) -> pd.Series:
    return series.notna()


def add_filter_columns(df: pd.DataFrame, *, staleness_ms: int, min_time_to_expiry_sec: int) -> pd.DataFrame:
    out = df.copy()
    out["time_to_expiry_sec"] = out["expiry_ts"] - (out["timestamp"] / 1000.0)
    out["exec_crossed"] = (
        out["yes_exec_bid"].notna()
        & out["yes_exec_ask"].notna()
        & (out["yes_exec_bid"] > out["yes_exec_ask"])
    )
    out["usable_buy_row"] = (
        out["yes_exec_ask"].notna()
        & (out["yes_exec_ask"] >= 0)
        & (out["yes_exec_ask"] <= 1)
        & ~out["exec_crossed"]
        & (out["yes_staleness_ms"] <= staleness_ms)
        & (out["no_staleness_ms"] <= staleness_ms)
        & (out["time_to_expiry_sec"] > min_time_to_expiry_sec)
    )
    out["usable_two_sided_row"] = (
        out["usable_buy_row"]
        & out["yes_exec_bid"].notna()
        & (out["yes_exec_bid"] <= out["yes_exec_ask"])
        & (out["yes_spread_exec"] <= 0.10)
    )
    return out


def market_reason(row: dict[str, Any]) -> str:
    reasons: list[str] = []
    if row["p95_staleness_ms"] is None or row["p95_staleness_ms"] > 10_000:
        reasons.append("p95_staleness_gt_10s")
    if row["crossed_rate"] > 0.01:
        reasons.append("crossed_rate_gt_1pct")
    if row["has_yes_exec_ask_rate"] < 0.15:
        reasons.append("buy_ask_rate_lt_15pct")
    if row["has_normalized_two_sided_rate"] < 0.30:
        reasons.append("two_sided_rate_lt_30pct")
    if row["median_exec_spread"] is None or row["median_exec_spread"] > 0.06:
        reasons.append("median_spread_gt_6c")
    return ";".join(reasons) if reasons else "none"


def tier_for(row: dict[str, Any]) -> str:
    p95_stale_ok = row["p95_staleness_ms"] is not None and row["p95_staleness_ms"] <= 10_000
    median_spread_ok = row["median_exec_spread"] is not None and row["median_exec_spread"] <= 0.06
    if (
        row["has_normalized_two_sided_rate"] >= 0.30
        and row["crossed_rate"] <= 0.005
        and p95_stale_ok
        and median_spread_ok
    ):
        return "A"
    if row["has_yes_exec_ask_rate"] >= 0.15 and row["crossed_rate"] <= 0.01 and p95_stale_ok:
        return "B"
    return "C"


def build_market_quality(df: pd.DataFrame) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    for market_id, group in df.groupby("market_id", sort=False):
        total = len(group)
        spread = pd.to_numeric(group["yes_spread_exec"], errors="coerce")
        crossed = group["exec_crossed"]
        p95_yes_stale = percentile(group["yes_staleness_ms"], 0.95)
        p95_no_stale = percentile(group["no_staleness_ms"], 0.95)
        stale_values = [value for value in (p95_yes_stale, p95_no_stale) if value is not None]
        row = {
            "market_id": market_id,
            "slug": str(group["slug"].dropna().iloc[0]) if group["slug"].notna().any() else "",
            "coin": str(group["coin"].dropna().iloc[0]) if group["coin"].notna().any() else "",
            "strike": float(group["strike"].dropna().iloc[0]) if group["strike"].notna().any() else None,
            "expiry_ts": int(group["expiry_ts"].dropna().iloc[0]) if group["expiry_ts"].notna().any() else None,
            "rows": total,
            "has_yes_exec_ask_rate": rate(is_not_null(group["yes_exec_ask"])),
            "has_yes_exec_bid_rate": rate(is_not_null(group["yes_exec_bid"])),
            "has_normalized_two_sided_rate": rate(group["has_normalized_two_sided"]),
            "crossed_rate": rate(crossed),
            "median_exec_spread": percentile(spread, 0.50),
            "p95_exec_spread": percentile(spread, 0.95),
            "p95_yes_staleness_ms": p95_yes_stale,
            "p95_no_staleness_ms": p95_no_stale,
            "p95_staleness_ms": max(stale_values) if stale_values else None,
            "usable_buy_rows": int(group["usable_buy_row"].sum()),
            "usable_two_sided_rows": int(group["usable_two_sided_row"].sum()),
        }
        row["tier"] = tier_for(row)
        row["drop_reason"] = market_reason(row) if row["tier"] == "C" else ""
        rows.append(row)
    quality = pd.DataFrame(rows)
    if quality.empty:
        return quality
    tier_order = {"A": 0, "B": 1, "C": 2}
    quality["_tier_order"] = quality["tier"].map(tier_order)
    quality = quality.sort_values(
        ["_tier_order", "usable_two_sided_rows", "usable_buy_rows", "has_normalized_two_sided_rate"],
        ascending=[True, False, False, False],
    ).drop(columns=["_tier_order"])
    return quality


def reason_summary(market_quality: pd.DataFrame) -> dict[str, int]:
    summary: dict[str, int] = {}
    dropped = market_quality[market_quality["tier"] == "C"]
    for reason_text in dropped["drop_reason"].fillna(""):
        for reason in str(reason_text).split(";"):
            reason = reason.strip()
            if not reason:
                continue
            summary[reason] = summary.get(reason, 0) + 1
    return dict(sorted(summary.items(), key=lambda item: (-item[1], item[0])))


def build_report(
    *,
    input_path: Path,
    output_dir: Path,
    df: pd.DataFrame,
    usable_buy: pd.DataFrame,
    usable_two_sided: pd.DataFrame,
    market_quality: pd.DataFrame,
    staleness_ms: int,
    min_time_to_expiry_sec: int,
) -> dict[str, Any]:
    return {
        "input": str(input_path),
        "output_dir": str(output_dir),
        "thresholds": {
            "staleness_ms": staleness_ms,
            "min_time_to_expiry_sec": min_time_to_expiry_sec,
            "tier_a_min_two_sided_rate": 0.30,
            "tier_a_max_crossed_rate": 0.005,
            "tier_a_max_median_spread": 0.06,
            "tier_b_min_buy_ask_rate": 0.15,
            "tier_b_max_crossed_rate": 0.01,
        },
        "rows": int(len(df)),
        "markets": int(df["market_id"].nunique()) if len(df) else 0,
        "usable_buy_rows": int(len(usable_buy)),
        "usable_two_sided_rows": int(len(usable_two_sided)),
        "tier_counts": {
            tier: int(count)
            for tier, count in market_quality["tier"].value_counts().reindex(["A", "B", "C"], fill_value=0).items()
        },
        "usable_buy_rows_by_coin": {
            str(coin): int(count)
            for coin, count in usable_buy.groupby("coin").size().sort_index().items()
        },
        "usable_two_sided_rows_by_coin": {
            str(coin): int(count)
            for coin, count in usable_two_sided.groupby("coin").size().sort_index().items()
        },
        "dropped_markets_reason_summary": reason_summary(market_quality),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", default=DEFAULT_INPUT)
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    parser.add_argument("--staleness-ms", type=int, default=10_000)
    parser.add_argument("--min-time-to-expiry-sec", type=int, default=600)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)
    output_dir = Path(args.out_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    df = pd.read_parquet(input_path)
    filtered = add_filter_columns(
        df,
        staleness_ms=args.staleness_ms,
        min_time_to_expiry_sec=args.min_time_to_expiry_sec,
    )
    usable_buy = filtered[filtered["usable_buy_row"]].copy()
    usable_two_sided = filtered[filtered["usable_two_sided_row"]].copy()
    market_quality = build_market_quality(filtered)

    market_quality.to_csv(output_dir / "market_quality.csv", index=False)
    usable_buy.to_parquet(output_dir / "usable_buy_rows.parquet", index=False)
    usable_two_sided.to_parquet(output_dir / "usable_two_sided_rows.parquet", index=False)
    report = build_report(
        input_path=input_path,
        output_dir=output_dir,
        df=filtered,
        usable_buy=usable_buy,
        usable_two_sided=usable_two_sided,
        market_quality=market_quality,
        staleness_ms=args.staleness_ms,
        min_time_to_expiry_sec=args.min_time_to_expiry_sec,
    )
    with (output_dir / "filter_report.json").open("w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(
        "wrote "
        f"markets={report['markets']} usable_buy_rows={report['usable_buy_rows']} "
        f"usable_two_sided_rows={report['usable_two_sided_rows']} out_dir={output_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

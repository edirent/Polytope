#!/usr/bin/env python3
"""Normalize binary Polymarket YES/NO L2 snapshots into YES-space.

Input is the PMXT sample/full parquet layout produced by
`fetch_poly_l2_snapshots.py`. For each binary market, this script builds a fixed
time grid, backward/asof joins YES and NO snapshots with a staleness tolerance,
and writes one normalized row per market per grid timestamp.
"""

from __future__ import annotations

import argparse
import glob
import json
import math
from pathlib import Path
from typing import Any

import pandas as pd


OUTPUT_PARQUET = "normalized_yes_space.parquet"
QUALITY_JSON = "normalization_quality.json"
VWAP_SIZES = (50, 100, 500)


def iso_from_ms(timestamp_ms: int) -> str:
    return pd.Timestamp(timestamp_ms, unit="ms", tz="UTC").isoformat()


def finite_or_none(value: Any) -> float | None:
    if value is None or pd.isna(value):
        return None
    parsed = float(value)
    return parsed if math.isfinite(parsed) else None


def min_non_null(values: list[tuple[float | None, str]]) -> tuple[float | None, str]:
    present = [(value, source) for value, source in values if value is not None]
    if not present:
        return None, ""
    value, source = min(present, key=lambda item: item[0])
    return value, source


def max_non_null(values: list[tuple[float | None, str]]) -> tuple[float | None, str]:
    present = [(value, source) for value, source in values if value is not None]
    if not present:
        return None, ""
    value, source = max(present, key=lambda item: item[0])
    return value, source


def ask_source_label(direct: float | None, synth: float | None, chosen: str) -> str:
    if direct is None and synth is None:
        return ""
    if direct is not None and synth is not None:
        return "both_direct_better" if chosen == "direct_yes" else "both_synthetic_better"
    return chosen


def read_l2_files(input_dir: Path) -> dict[str, dict[str, Path]]:
    files = sorted(glob.glob(str(input_dir / "**/*.parquet"), recursive=True))
    grouped: dict[str, dict[str, Path]] = {}
    for file_name in files:
        path = Path(file_name)
        stem = path.stem
        if stem.endswith("_yes"):
            market_id = stem[: -len("_yes")]
            grouped.setdefault(market_id, {})["yes"] = path
        elif stem.endswith("_no"):
            market_id = stem[: -len("_no")]
            grouped.setdefault(market_id, {})["no"] = path
    return grouped


def prep_side(path: Path, side: str) -> pd.DataFrame:
    df = pd.read_parquet(path)
    if df.empty or "timestamp" not in df.columns:
        return pd.DataFrame()
    df = df.sort_values("timestamp")
    df = df.drop_duplicates("timestamp", keep="last")
    rename = {
        "timestamp": f"{side}_snapshot_ts",
        "best_bid": f"{side}_direct_bid",
        "best_ask": f"{side}_direct_ask",
        "vwap_buy_50": f"{side}_vwap_buy_50",
        "vwap_buy_100": f"{side}_vwap_buy_100",
        "vwap_buy_500": f"{side}_vwap_buy_500",
    }
    keep = [
        "timestamp",
        "market_id",
        "slug",
        "coin",
        "strike",
        "expiry_ts",
        "best_bid",
        "best_ask",
        "vwap_buy_50",
        "vwap_buy_100",
        "vwap_buy_500",
    ]
    cols = [col for col in keep if col in df.columns]
    return df[cols].rename(columns=rename)


def make_grid(yes_df: pd.DataFrame, no_df: pd.DataFrame, step_ms: int) -> pd.DataFrame:
    start = max(int(yes_df["yes_snapshot_ts"].min()), int(no_df["no_snapshot_ts"].min()))
    end = min(int(yes_df["yes_snapshot_ts"].max()), int(no_df["no_snapshot_ts"].max()))
    if end < start:
        return pd.DataFrame({"timestamp": pd.Series(dtype="int64")})
    timestamps = list(range(start, end + 1, step_ms))
    if timestamps and timestamps[-1] != end:
        timestamps.append(end)
    return pd.DataFrame({"timestamp": timestamps})


def asof_join_side(grid: pd.DataFrame, side_df: pd.DataFrame, side: str, tolerance_ms: int) -> pd.DataFrame:
    joined = pd.merge_asof(
        grid.sort_values("timestamp"),
        side_df.sort_values(f"{side}_snapshot_ts"),
        left_on="timestamp",
        right_on=f"{side}_snapshot_ts",
        direction="backward",
        tolerance=tolerance_ms,
    )
    staleness_col = f"{side}_staleness_ms"
    joined[staleness_col] = joined["timestamp"] - joined[f"{side}_snapshot_ts"]
    stale = joined[staleness_col].isna() | (joined[staleness_col] > tolerance_ms)
    side_cols = [col for col in joined.columns if col.startswith(f"{side}_") and col != staleness_col]
    joined.loc[stale, side_cols] = pd.NA
    return joined


def pick_meta(yes_row: pd.Series, no_row: pd.Series, field: str) -> Any:
    yes_value = yes_row.get(field)
    if yes_value is not None and not pd.isna(yes_value):
        return yes_value
    no_value = no_row.get(field)
    if no_value is not None and not pd.isna(no_value):
        return no_value
    return None


def normalize_market(market_id: str, yes_path: Path, no_path: Path, step_ms: int, tolerance_ms: int) -> pd.DataFrame:
    yes_df = prep_side(yes_path, "yes")
    no_df = prep_side(no_path, "no")
    if yes_df.empty or no_df.empty:
        return pd.DataFrame()
    grid = make_grid(yes_df, no_df, step_ms)
    if grid.empty:
        return pd.DataFrame()
    meta_source = yes_df.iloc[0] if not yes_df.empty else no_df.iloc[0]
    static_meta = {
        "slug": meta_source.get("slug"),
        "coin": meta_source.get("coin"),
        "strike": meta_source.get("strike"),
        "expiry_ts": meta_source.get("expiry_ts"),
    }

    yes_join = asof_join_side(grid, yes_df, "yes", tolerance_ms)
    no_join = asof_join_side(grid, no_df, "no", tolerance_ms)

    rows: list[dict[str, Any]] = []
    for idx in range(len(grid)):
        yes_row = yes_join.iloc[idx]
        no_row = no_join.iloc[idx]
        timestamp = int(grid.iloc[idx]["timestamp"])

        yes_direct_bid = finite_or_none(yes_row.get("yes_direct_bid"))
        yes_direct_ask = finite_or_none(yes_row.get("yes_direct_ask"))
        no_direct_bid = finite_or_none(no_row.get("no_direct_bid"))
        no_direct_ask = finite_or_none(no_row.get("no_direct_ask"))

        yes_synth_bid = 1.0 - no_direct_ask if no_direct_ask is not None else None
        yes_synth_ask = 1.0 - no_direct_bid if no_direct_bid is not None else None
        yes_exec_bid, yes_exec_bid_source = max_non_null(
            [
                (yes_direct_bid, "direct_yes"),
                (yes_synth_bid, "synthetic_from_no_ask"),
            ]
        )
        yes_exec_ask, raw_ask_source = min_non_null(
            [
                (yes_direct_ask, "direct_yes"),
                (yes_synth_ask, "synthetic_from_no_bid"),
            ]
        )
        yes_exec_ask_source = ask_source_label(yes_direct_ask, yes_synth_ask, raw_ask_source)
        yes_spread = yes_exec_ask - yes_exec_bid if yes_exec_bid is not None and yes_exec_ask is not None else None
        yes_mid = (yes_exec_bid + yes_exec_ask) / 2.0 if yes_spread is not None else None

        buy_costs: dict[str, float | None] = {}
        buy_sources: dict[str, str] = {}
        for size in VWAP_SIZES:
            direct_cost = finite_or_none(yes_row.get(f"yes_vwap_buy_{size}"))
            # First version: synthetic VWAP is top-of-book only. It is useful as
            # a sanity signal but should not be treated as full-depth executable VWAP.
            synthetic_cost = yes_synth_ask
            cost, source = min_non_null(
                [
                    (direct_cost, "direct_yes_vwap"),
                    (synthetic_cost, "synthetic_from_no_bid_top"),
                ]
            )
            buy_costs[f"buy_yes_cost_{size}"] = cost
            buy_sources[f"buy_yes_cost_source_{size}"] = source

        no_buy_cost = finite_or_none(no_row.get("no_vwap_buy_50"))
        row = {
            "timestamp": timestamp,
            "datetime": iso_from_ms(timestamp),
            "market_id": market_id,
            "slug": static_meta["slug"],
            "coin": static_meta["coin"],
            "strike": static_meta["strike"],
            "expiry_ts": static_meta["expiry_ts"],
            "yes_direct_bid": yes_direct_bid,
            "yes_direct_ask": yes_direct_ask,
            "no_direct_bid": no_direct_bid,
            "no_direct_ask": no_direct_ask,
            "yes_synth_bid_from_no_ask": yes_synth_bid,
            "yes_synth_ask_from_no_bid": yes_synth_ask,
            "yes_exec_bid": yes_exec_bid,
            "yes_exec_ask": yes_exec_ask,
            "yes_exec_bid_source": yes_exec_bid_source,
            "yes_exec_ask_source": yes_exec_ask_source,
            "yes_mid_exec": yes_mid,
            "yes_spread_exec": yes_spread,
            **buy_costs,
            **buy_sources,
            "has_direct_yes_two_sided": yes_direct_bid is not None and yes_direct_ask is not None,
            "has_direct_no_two_sided": no_direct_bid is not None and no_direct_ask is not None,
            "has_normalized_two_sided": yes_exec_bid is not None and yes_exec_ask is not None,
            "has_buy_yes_cost": any(buy_costs[f"buy_yes_cost_{size}"] is not None for size in VWAP_SIZES),
            "has_buy_no_cost": no_buy_cost is not None,
            "yes_staleness_ms": finite_or_none(yes_row.get("yes_staleness_ms")),
            "no_staleness_ms": finite_or_none(no_row.get("no_staleness_ms")),
        }
        rows.append(row)
    return pd.DataFrame(rows)


def rate(series: pd.Series) -> float:
    return float(series.fillna(False).mean()) if len(series) else 0.0


def percentile(series: pd.Series, q: float) -> float | None:
    values = pd.to_numeric(series, errors="coerce").dropna()
    if values.empty:
        return None
    return float(values.quantile(q))


def source_breakdown(series: pd.Series) -> dict[str, int]:
    values = series.fillna("").astype(str)
    keys = [
        "direct_yes",
        "synthetic_from_no_bid",
        "both_direct_better",
        "both_synthetic_better",
        "",
    ]
    counts = values.value_counts().to_dict()
    return {key if key else "missing": int(counts.get(key, 0)) for key in keys}


def build_quality(df: pd.DataFrame, *, input_dir: Path, output_dir: Path, step_ms: int, tolerance_ms: int) -> dict[str, Any]:
    spread = pd.to_numeric(df.get("yes_spread_exec"), errors="coerce")
    exec_bid = pd.to_numeric(df.get("yes_exec_bid"), errors="coerce")
    exec_ask = pd.to_numeric(df.get("yes_exec_ask"), errors="coerce")
    crossed = (exec_bid.notna() & exec_ask.notna() & (exec_bid > exec_ask))
    negative_spread = spread.notna() & (spread < 0)

    by_market: dict[str, Any] = {}
    for market_id, group in df.groupby("market_id"):
        group_spread = pd.to_numeric(group["yes_spread_exec"], errors="coerce")
        by_market[str(market_id)] = {
            "rows": int(len(group)),
            "slug": str(group["slug"].dropna().iloc[0]) if group["slug"].notna().any() else "",
            "coin": str(group["coin"].dropna().iloc[0]) if group["coin"].notna().any() else "",
            "has_yes_exec_ask_rate": rate(group["yes_exec_ask"].notna()),
            "has_normalized_two_sided_rate": rate(group["has_normalized_two_sided"]),
            "exec_crossed_count": int((group["yes_exec_bid"].notna() & group["yes_exec_ask"].notna() & (group["yes_exec_bid"] > group["yes_exec_ask"])).sum()),
            "median_exec_spread": percentile(group_spread, 0.50),
            "exec_ask_source_breakdown": source_breakdown(group["yes_exec_ask_source"]),
        }

    return {
        "input_dir": str(input_dir),
        "output_dir": str(output_dir),
        "step_ms": step_ms,
        "tolerance_ms": tolerance_ms,
        "rows": int(len(df)),
        "markets": int(df["market_id"].nunique()) if len(df) else 0,
        "has_yes_direct_ask_rate": rate(df["yes_direct_ask"].notna()),
        "has_yes_direct_bid_rate": rate(df["yes_direct_bid"].notna()),
        "has_no_direct_ask_rate": rate(df["no_direct_ask"].notna()),
        "has_no_direct_bid_rate": rate(df["no_direct_bid"].notna()),
        "has_yes_synth_ask_rate": rate(df["yes_synth_ask_from_no_bid"].notna()),
        "has_yes_synth_bid_rate": rate(df["yes_synth_bid_from_no_ask"].notna()),
        "has_yes_exec_ask_rate": rate(df["yes_exec_ask"].notna()),
        "has_yes_exec_bid_rate": rate(df["yes_exec_bid"].notna()),
        "has_normalized_two_sided_rate": rate(df["has_normalized_two_sided"]),
        "exec_crossed_count": int(crossed.sum()),
        "exec_negative_spread_count": int(negative_spread.sum()),
        "median_exec_spread": percentile(spread, 0.50),
        "p95_exec_spread": percentile(spread, 0.95),
        "exec_ask_source_breakdown": source_breakdown(df["yes_exec_ask_source"]),
        "staleness": {
            "yes_median_staleness_ms": percentile(df["yes_staleness_ms"], 0.50),
            "no_median_staleness_ms": percentile(df["no_staleness_ms"], 0.50),
            "p95_yes_staleness_ms": percentile(df["yes_staleness_ms"], 0.95),
            "p95_no_staleness_ms": percentile(df["no_staleness_ms"], 0.95),
        },
        "by_market": by_market,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", default="runs/edge_discovery/latest_14d/poly_l2_sample")
    parser.add_argument("--out-dir", default="runs/edge_discovery/latest_14d/poly_l2_sample_normalized")
    parser.add_argument("--step-sec", type=int, default=5)
    parser.add_argument("--tolerance-sec", "--staleness-sec", dest="tolerance_sec", type=int, default=10)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_dir = Path(args.input_dir)
    output_dir = Path(args.out_dir)
    step_ms = args.step_sec * 1000
    tolerance_ms = args.tolerance_sec * 1000
    grouped = read_l2_files(input_dir)

    frames: list[pd.DataFrame] = []
    for market_id, paths in sorted(grouped.items()):
        if "yes" not in paths or "no" not in paths:
            continue
        frame = normalize_market(market_id, paths["yes"], paths["no"], step_ms, tolerance_ms)
        if not frame.empty:
            frames.append(frame)

    normalized = pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()
    output_dir.mkdir(parents=True, exist_ok=True)
    normalized.to_parquet(output_dir / OUTPUT_PARQUET, index=False)
    quality = build_quality(normalized, input_dir=input_dir, output_dir=output_dir, step_ms=step_ms, tolerance_ms=tolerance_ms)
    with (output_dir / QUALITY_JSON).open("w", encoding="utf-8") as handle:
        json.dump(quality, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"wrote rows={len(normalized)} markets={normalized['market_id'].nunique() if len(normalized) else 0} out_dir={output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

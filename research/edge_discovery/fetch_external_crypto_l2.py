#!/usr/bin/env python3
"""Fetch external crypto spot prices on a 5s grid for fair-value research.

First version is intentionally SOL-only by default. Public Coinbase/Kraken
historical endpoints expose minute candles rather than historical L1 quotes, so
the output marks `has_l1_quote=false` and uses candle close as `price`/`mid`.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

import pandas as pd


COINBASE_PRODUCTS = {"SOL": "SOL-USD"}
KRAKEN_PAIRS = {"SOL": "SOLUSD"}
SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0


def http_json(url: str, timeout_s: int = 30) -> Any:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "PolytopeEdgeDiscovery/0.1",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return json.loads(response.read().decode("utf-8"))


def iso_from_ms(timestamp_ms: int) -> str:
    return pd.Timestamp(timestamp_ms, unit="ms", tz="UTC").isoformat()


def read_sol_window(poly_rows_path: Path, buffer_before_hours: float, buffer_after_hours: float) -> tuple[int, int]:
    df = pd.read_parquet(poly_rows_path)
    sol = df[df["coin"] == "SOL"]
    if sol.empty:
        raise RuntimeError(f"no SOL rows in {poly_rows_path}")
    start_ms = int(sol["timestamp"].min() - buffer_before_hours * 3600 * 1000)
    end_ms = int(sol["timestamp"].max() + buffer_after_hours * 3600 * 1000)
    return start_ms, end_ms


def fetch_coinbase_candles(coin: str, start_ms: int, end_ms: int) -> pd.DataFrame:
    product = COINBASE_PRODUCTS[coin]
    frames: list[pd.DataFrame] = []
    granularity = 60
    # Coinbase Exchange candles allow up to 300 buckets per request.
    chunk_ms = 300 * granularity * 1000
    cursor = start_ms
    while cursor < end_ms:
        chunk_end = min(cursor + chunk_ms, end_ms)
        query = urllib.parse.urlencode(
            {
                "start": pd.Timestamp(cursor, unit="ms", tz="UTC").isoformat(),
                "end": pd.Timestamp(chunk_end, unit="ms", tz="UTC").isoformat(),
                "granularity": granularity,
            }
        )
        url = f"https://api.exchange.coinbase.com/products/{product}/candles?{query}"
        data = http_json(url)
        if not isinstance(data, list):
            raise RuntimeError(f"unexpected Coinbase response for {coin}: {type(data)}")
        rows = [
            {
                "timestamp": int(item[0]) * 1000,
                "low": float(item[1]),
                "high": float(item[2]),
                "open": float(item[3]),
                "close": float(item[4]),
                "volume": float(item[5]),
            }
            for item in data
            if isinstance(item, list) and len(item) >= 6
        ]
        if rows:
            frames.append(pd.DataFrame(rows))
        cursor = chunk_end
        time.sleep(0.05)
    if not frames:
        raise RuntimeError(f"Coinbase returned no candles for {coin}")
    df = pd.concat(frames, ignore_index=True).drop_duplicates("timestamp").sort_values("timestamp")
    return df[(df["timestamp"] >= start_ms) & (df["timestamp"] <= end_ms)]


def fetch_kraken_candles(coin: str, start_ms: int, end_ms: int) -> pd.DataFrame:
    pair = KRAKEN_PAIRS[coin]
    since_sec = max(0, int(start_ms / 1000) - 60)
    url = f"https://api.kraken.com/0/public/OHLC?{urllib.parse.urlencode({'pair': pair, 'interval': 1, 'since': since_sec})}"
    data = http_json(url)
    if not isinstance(data, dict) or data.get("error"):
        raise RuntimeError(f"unexpected Kraken response for {coin}: {data}")
    result = data.get("result")
    if not isinstance(result, dict):
        raise RuntimeError(f"missing Kraken result for {coin}")
    key = next((k for k in result if k != "last"), None)
    if key is None:
        raise RuntimeError(f"missing Kraken OHLC payload for {coin}")
    rows = [
        {
            "timestamp": int(float(item[0])) * 1000,
            "open": float(item[1]),
            "high": float(item[2]),
            "low": float(item[3]),
            "close": float(item[4]),
            "volume": float(item[6]),
        }
        for item in result[key]
        if isinstance(item, list) and len(item) >= 7
    ]
    df = pd.DataFrame(rows).drop_duplicates("timestamp").sort_values("timestamp")
    return df[(df["timestamp"] >= start_ms) & (df["timestamp"] <= end_ms)]


def candles_to_5s(candles: pd.DataFrame, *, coin: str, venue: str, symbol: str, start_ms: int, end_ms: int) -> pd.DataFrame:
    grid = pd.DataFrame({"timestamp": list(range(start_ms, end_ms + 1, 5000))})
    candles = candles.sort_values("timestamp")
    joined = pd.merge_asof(grid, candles[["timestamp", "close"]], on="timestamp", direction="backward", tolerance=120_000)
    joined = joined.rename(columns={"close": "price"})
    joined["mid"] = joined["price"]
    joined["best_bid"] = pd.NA
    joined["best_ask"] = pd.NA
    joined["spread"] = pd.NA
    joined["coin"] = coin
    joined["venue"] = venue
    joined["symbol"] = symbol
    joined["datetime"] = joined["timestamp"].map(iso_from_ms)
    joined["has_l1_quote"] = False
    joined = joined.dropna(subset=["mid"]).copy()
    joined["return_5s"] = (joined["mid"] / joined["mid"].shift(1)).apply(lambda value: math.log(value) if pd.notna(value) and value > 0 else math.nan)
    for seconds in (30, 60):
        periods = seconds // 5
        joined[f"return_{seconds}s"] = (joined["mid"] / joined["mid"].shift(periods)).apply(
            lambda value: math.log(value) if pd.notna(value) and value > 0 else math.nan
        )
    for label, seconds in (("5m", 300), ("1h", 3600), ("24h", 86400)):
        window = seconds // 5
        joined[f"realized_vol_{label}"] = joined["return_5s"].rolling(window=window, min_periods=max(2, window // 2)).std() * math.sqrt(SECONDS_PER_YEAR / 5.0)
    ordered = [
        "timestamp",
        "datetime",
        "coin",
        "venue",
        "symbol",
        "best_bid",
        "best_ask",
        "price",
        "mid",
        "spread",
        "return_5s",
        "return_30s",
        "return_60s",
        "realized_vol_5m",
        "realized_vol_1h",
        "realized_vol_24h",
        "has_l1_quote",
    ]
    return joined[ordered]


def quality_report(df: pd.DataFrame, *, coin: str, venue: str, start_ms: int, end_ms: int) -> dict[str, Any]:
    intervals = df["timestamp"].diff().dropna() / 1000.0
    return {
        "coin": coin,
        "venue": venue,
        "requested_start_ts": start_ms,
        "requested_end_ts": end_ms,
        "rows": int(len(df)),
        "first_ts": int(df["timestamp"].min()) if len(df) else None,
        "last_ts": int(df["timestamp"].max()) if len(df) else None,
        "coverage_hours": (float(df["timestamp"].max() - df["timestamp"].min()) / 3_600_000.0) if len(df) > 1 else 0.0,
        "median_interval_sec": float(intervals.median()) if len(intervals) else None,
        "p95_interval_sec": float(intervals.quantile(0.95)) if len(intervals) else None,
        "mid_null_rate": float(df["mid"].isna().mean()) if len(df) else None,
        "realized_vol_1h_null_rate": float(df["realized_vol_1h"].isna().mean()) if len(df) else None,
        "has_l1_quote": bool(df["has_l1_quote"].any()) if len(df) else False,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--poly-rows", default="runs/edge_discovery/latest_14d/poly_l2_sample_atm_filtered/usable_buy_rows.parquet")
    parser.add_argument("--out-dir", default="runs/edge_discovery/latest_14d/external_crypto_5s")
    parser.add_argument("--coin", default="SOL", choices=["SOL"])
    parser.add_argument("--buffer-before-hours", type=float, default=24.0)
    parser.add_argument("--buffer-after-hours", type=float, default=1.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    start_ms, end_ms = read_sol_window(Path(args.poly_rows), args.buffer_before_hours, args.buffer_after_hours)
    venue = "coinbase"
    symbol = COINBASE_PRODUCTS[args.coin]
    try:
        candles = fetch_coinbase_candles(args.coin, start_ms, end_ms)
    except Exception:
        venue = "kraken"
        symbol = KRAKEN_PAIRS[args.coin]
        candles = fetch_kraken_candles(args.coin, start_ms, end_ms)
    df = candles_to_5s(candles, coin=args.coin, venue=venue, symbol=symbol, start_ms=start_ms, end_ms=end_ms)
    df.to_parquet(out_dir / f"{args.coin}.parquet", index=False)
    report = quality_report(df, coin=args.coin, venue=venue, start_ms=start_ms, end_ms=end_ms)
    with (out_dir / "quality_report.json").open("w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"wrote rows={len(df)} coin={args.coin} venue={venue} out_dir={out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

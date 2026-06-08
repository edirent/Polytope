#!/usr/bin/env python3
"""Select ATM, liquid BTC/ETH/SOL binary markets for PMXT L2 sampling."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


COINS = ("BTC", "ETH", "SOL")
COINBASE_PRODUCTS = {
    "BTC": "BTC-USD",
    "ETH": "ETH-USD",
    "SOL": "SOL-USD",
}
KRAKEN_PAIRS = {
    "BTC": "XBTUSD",
    "ETH": "ETHUSD",
    "SOL": "SOLUSD",
}
BASE_FIELDS = [
    "market_id",
    "slug",
    "question",
    "coin",
    "strike",
    "expiry_ts",
    "yes_token_id",
    "no_token_id",
    "active",
    "closed",
    "volume",
    "liquidity",
]
EXTRA_FIELDS = [
    "spot",
    "abs_log_moneyness",
    "hours_to_expiry",
    "selection_rank",
]


def now_utc() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def http_json(url: str, timeout_s: int = 15) -> Any:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "PolytopeEdgeDiscovery/0.1",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return json.loads(response.read().decode("utf-8"))


def number(value: Any, default: float = 0.0) -> float:
    if value in (None, ""):
        return default
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    return parsed if math.isfinite(parsed) else default


def parse_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in {"1", "true", "yes"}


def fetch_coinbase_spot(coin: str) -> float | None:
    product = COINBASE_PRODUCTS[coin]
    data = http_json(f"https://api.exchange.coinbase.com/products/{product}/ticker")
    price = number(data.get("price"), default=math.nan) if isinstance(data, dict) else math.nan
    return price if math.isfinite(price) and price > 0 else None


def fetch_kraken_spot(coin: str) -> float | None:
    pair = KRAKEN_PAIRS[coin]
    data = http_json(f"https://api.kraken.com/0/public/Ticker?pair={pair}")
    if not isinstance(data, dict) or data.get("error"):
        return None
    result = data.get("result")
    if not isinstance(result, dict) or not result:
        return None
    first = next(iter(result.values()))
    price = number((first.get("c") or [None])[0], default=math.nan) if isinstance(first, dict) else math.nan
    return price if math.isfinite(price) and price > 0 else None


def get_spots(args: argparse.Namespace) -> dict[str, float]:
    overrides = {
        "BTC": args.btc_spot,
        "ETH": args.eth_spot,
        "SOL": args.sol_spot,
    }
    spots: dict[str, float] = {}
    for coin in args.coins:
        override = overrides.get(coin)
        if override is not None:
            spots[coin] = float(override)
            continue
        spot: float | None = None
        for fetcher in (fetch_coinbase_spot, fetch_kraken_spot):
            try:
                spot = fetcher(coin)
            except (urllib.error.URLError, TimeoutError, ValueError, KeyError, TypeError):
                spot = None
            if spot is not None:
                break
        if spot is None:
            raise RuntimeError(f"could not fetch spot for {coin}; pass --{coin.lower()}-spot")
        spots[coin] = spot
    return spots


def read_universe(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def enrich_row(row: dict[str, Any], spot: float, as_of_ts: int) -> dict[str, Any] | None:
    strike = number(row.get("strike"), default=math.nan)
    expiry_ts = int(number(row.get("expiry_ts"), default=0.0))
    if not math.isfinite(strike) or strike <= 0 or expiry_ts <= 0:
        return None
    enriched = dict(row)
    enriched["spot"] = spot
    enriched["abs_log_moneyness"] = abs(math.log(spot / strike))
    enriched["hours_to_expiry"] = (expiry_ts - as_of_ts) / 3600.0
    return enriched


def select_markets(rows: list[dict[str, Any]], spots: dict[str, float], args: argparse.Namespace) -> list[dict[str, Any]]:
    as_of_ts = int(now_utc().timestamp())
    selected: list[dict[str, Any]] = []
    for coin in args.coins:
        candidates: list[dict[str, Any]] = []
        for row in rows:
            if row.get("coin") != coin:
                continue
            if not parse_bool(row.get("active")) or parse_bool(row.get("closed")):
                continue
            enriched = enrich_row(row, spots[coin], as_of_ts)
            if enriched is None:
                continue
            hours = float(enriched["hours_to_expiry"])
            if hours < args.min_expiry_hours or hours > args.max_expiry_hours:
                continue
            candidates.append(enriched)

        candidates.sort(
            key=lambda row: (
                float(row["abs_log_moneyness"]),
                -number(row.get("liquidity")),
                -number(row.get("volume")),
                float(row["hours_to_expiry"]),
            )
        )
        for rank, row in enumerate(candidates[: args.markets_per_coin], start=1):
            out = dict(row)
            out["selection_rank"] = rank
            selected.append(out)
    return selected


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=BASE_FIELDS + EXTRA_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            formatted = dict(row)
            for field in ("spot", "abs_log_moneyness", "hours_to_expiry"):
                formatted[field] = f"{float(row[field]):.10g}"
            writer.writerow(formatted)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--universe", default="runs/edge_discovery/latest_14d/markets_universe.csv")
    parser.add_argument("--out", default="runs/edge_discovery/latest_14d/selected_l2_sample_markets.csv")
    parser.add_argument("--coins", nargs="+", default=list(COINS), choices=list(COINS))
    parser.add_argument("--markets-per-coin", type=int, default=5)
    parser.add_argument("--min-expiry-hours", type=float, default=1.0)
    parser.add_argument("--max-expiry-hours", type=float, default=72.0)
    parser.add_argument("--btc-spot", type=float, default=None)
    parser.add_argument("--eth-spot", type=float, default=None)
    parser.add_argument("--sol-spot", type=float, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = read_universe(Path(args.universe))
    spots = get_spots(args)
    selected = select_markets(rows, spots, args)
    write_csv(Path(args.out), selected)
    print(f"spots={spots}")
    print(f"selected={len(selected)} out={args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Fetch historical Polymarket L2 snapshots from PMXT for edge discovery.

The script intentionally starts small: select a bounded set of markets from the
universe CSV, fetch YES and NO reconstructed L2 books, write parquet files, and
emit a chunk-level manifest plus quality report. PMXT archive timestamps are
treated as authoritative; this script does not assume snapshots land exactly on
the requested sampling grid.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pandas as pd


PMXT_FETCH_ORDER_BOOK_URL = "https://api.pmxt.dev/api/polymarket/fetchOrderBook"
OUTCOMES = ("yes", "no")


@dataclass(frozen=True)
class MarketRow:
    market_id: str
    slug: str
    question: str
    coin: str
    strike: float
    expiry_ts: int
    yes_token_id: str
    no_token_id: str
    active: bool
    closed: bool
    volume: float
    liquidity: float


@dataclass(frozen=True)
class ChunkResult:
    rows: list[dict[str, Any]]
    status: str
    error: str
    retry_count: int


def now_utc() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def parse_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in {"1", "true", "yes"}


def number(value: Any, default: float = 0.0) -> float:
    if value in (None, ""):
        return default
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    return parsed if math.isfinite(parsed) else default


def iso_from_ms(timestamp_ms: int | None) -> str:
    if timestamp_ms is None:
        return ""
    return dt.datetime.fromtimestamp(timestamp_ms / 1000.0, tz=dt.timezone.utc).isoformat()


def parse_datetime_to_ms(value: Any) -> int | None:
    if value in (None, ""):
        return None
    if isinstance(value, dt.datetime):
        parsed = value
    else:
        text = str(value).strip()
        if not text:
            return None
        if text.endswith("Z"):
            text = text[:-1] + "+00:00"
        try:
            parsed = dt.datetime.fromisoformat(text)
        except ValueError:
            return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return int(parsed.astimezone(dt.timezone.utc).timestamp() * 1000)


def coerce_timestamp_ms(value: Any) -> int | None:
    if value in (None, ""):
        return None
    if isinstance(value, dt.datetime):
        return parse_datetime_to_ms(value)
    if isinstance(value, str):
        parsed_dt = parse_datetime_to_ms(value)
        if parsed_dt is not None:
            return parsed_dt
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(numeric):
        return None
    # PMXT/CCXT-style timestamps are usually ms. Seconds are still accepted.
    return int(numeric if numeric > 10_000_000_000 else numeric * 1000)


def attr_or_key(value: Any, *names: str) -> Any:
    if isinstance(value, dict):
        for name in names:
            if name in value:
                return value[name]
        return None
    for name in names:
        if hasattr(value, name):
            return getattr(value, name)
    return None


def get_price(level: Any) -> float | None:
    if isinstance(level, (list, tuple)) and level:
        return number(level[0], default=math.nan)
    value = attr_or_key(level, "price", "px")
    parsed = number(value, default=math.nan)
    return parsed if math.isfinite(parsed) else None


def get_size(level: Any) -> float | None:
    if isinstance(level, (list, tuple)) and len(level) >= 2:
        return number(level[1], default=math.nan)
    value = attr_or_key(level, "size", "amount", "quantity", "qty")
    parsed = number(value, default=math.nan)
    return parsed if math.isfinite(parsed) else None


def normalize_levels(levels: Any) -> list[dict[str, float]]:
    normalized: list[dict[str, float]] = []
    if not levels:
        return normalized
    for level in levels:
        price = get_price(level)
        size = get_size(level)
        if price is None or size is None:
            continue
        if price < 0 or size < 0:
            continue
        normalized.append({"price": price, "size": size})
    return normalized


def vwap_buy(asks: list[dict[str, float]], target_size: float) -> float | None:
    remaining = target_size
    cost = 0.0
    filled = 0.0
    for level in sorted(asks, key=lambda item: item["price"]):
        take = min(level["size"], remaining)
        cost += level["price"] * take
        filled += take
        remaining -= take
        if remaining <= 0:
            break
    if filled < target_size:
        return None
    return cost / filled


def depth_bid_within(best_bid: float | None, bids: list[dict[str, float]], cents: float) -> float | None:
    if best_bid is None:
        return None
    return sum(level["size"] for level in bids if level["price"] >= best_bid - cents)


def depth_ask_within(best_ask: float | None, asks: list[dict[str, float]], cents: float) -> float | None:
    if best_ask is None:
        return None
    return sum(level["size"] for level in asks if level["price"] <= best_ask + cents)


def book_timestamp_ms(book: Any) -> int | None:
    for name in ("timestamp", "ts", "time", "datetime"):
        parsed = coerce_timestamp_ms(attr_or_key(book, name))
        if parsed is not None:
            return parsed
    return None


def summarize_book(book: Any, market: MarketRow, outcome: str) -> dict[str, Any]:
    bids = normalize_levels(attr_or_key(book, "bids") or [])
    asks = normalize_levels(attr_or_key(book, "asks") or [])
    best_bid = max((level["price"] for level in bids), default=None)
    best_ask = min((level["price"] for level in asks), default=None)
    spread = best_ask - best_bid if best_bid is not None and best_ask is not None else None
    mid = (best_bid + best_ask) / 2.0 if best_bid is not None and best_ask is not None else None
    timestamp_ms = book_timestamp_ms(book)

    return {
        "timestamp": timestamp_ms,
        "datetime": iso_from_ms(timestamp_ms),
        "market_id": market.market_id,
        "slug": market.slug,
        "coin": market.coin,
        "strike": market.strike,
        "expiry_ts": market.expiry_ts,
        "outcome": outcome,
        "best_bid": best_bid,
        "best_ask": best_ask,
        "bid_size_1": next((level["size"] for level in sorted(bids, key=lambda item: item["price"], reverse=True)), None),
        "ask_size_1": next((level["size"] for level in sorted(asks, key=lambda item: item["price"])), None),
        "spread": spread,
        "mid": mid,
        "depth_bid_1c": depth_bid_within(best_bid, bids, 0.01),
        "depth_ask_1c": depth_ask_within(best_ask, asks, 0.01),
        "depth_bid_3c": depth_bid_within(best_bid, bids, 0.03),
        "depth_ask_3c": depth_ask_within(best_ask, asks, 0.03),
        "depth_bid_5c": depth_bid_within(best_bid, bids, 0.05),
        "depth_ask_5c": depth_ask_within(best_ask, asks, 0.05),
        "vwap_buy_50": vwap_buy(asks, 50.0),
        "vwap_buy_100": vwap_buy(asks, 100.0),
        "vwap_buy_500": vwap_buy(asks, 500.0),
        "num_bids": len(bids),
        "num_asks": len(asks),
        "raw_bids_json": json.dumps(bids, separators=(",", ":")),
        "raw_asks_json": json.dumps(asks, separators=(",", ":")),
    }


def read_universe(path: Path) -> list[MarketRow]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = []
        for row in csv.DictReader(handle):
            rows.append(
                MarketRow(
                    market_id=str(row["market_id"]),
                    slug=str(row["slug"]),
                    question=str(row["question"]),
                    coin=str(row["coin"]),
                    strike=number(row["strike"], default=math.nan),
                    expiry_ts=int(number(row["expiry_ts"], default=0.0)),
                    yes_token_id=str(row["yes_token_id"]),
                    no_token_id=str(row["no_token_id"]),
                    active=parse_bool(row["active"]),
                    closed=parse_bool(row["closed"]),
                    volume=number(row["volume"]),
                    liquidity=number(row["liquidity"]),
                )
            )
        return rows


def select_markets(
    universe: list[MarketRow],
    coins: list[str],
    markets_per_coin: int,
    as_of_ts: int,
) -> list[MarketRow]:
    selected: list[MarketRow] = []
    for coin in coins:
        candidates = [row for row in universe if row.coin == coin and (row.active or row.closed)]
        candidates.sort(
            key=lambda row: (
                row.expiry_ts >= as_of_ts,
                row.active and not row.closed,
                row.liquidity,
                row.volume,
                -abs(row.expiry_ts - as_of_ts),
            ),
            reverse=True,
        )
        selected.extend(candidates[:markets_per_coin])
    return selected


class PmxtClient:
    def __init__(self, api_key: str, transport: str) -> None:
        self.api_key = api_key
        self.transport = transport
        self._poly = None
        if transport in {"auto", "pmxt"}:
            try:
                import pmxt  # type: ignore[import-not-found]
            except ModuleNotFoundError:
                if transport == "pmxt":
                    raise
            else:
                self.transport = "pmxt"
                self._poly = pmxt.Polymarket(pmxt_api_key=api_key)
                return
        if transport in {"auto", "http"}:
            self.transport = "http"
            return
        raise ValueError(f"unsupported PMXT transport: {transport}")

    def fetch_order_book(self, market_id: str, params: dict[str, Any]) -> list[Any]:
        if self.transport == "pmxt":
            result = self._poly.fetch_order_book(market_id, params=params)
        else:
            body = json.dumps({"args": [market_id, None, params]}, separators=(",", ":")).encode("utf-8")
            request = urllib.request.Request(
                PMXT_FETCH_ORDER_BOOK_URL,
                data=body,
                method="POST",
                headers={
                    "Accept": "application/json",
                    "Authorization": f"Bearer {self.api_key}",
                    "Content-Type": "application/json",
                    "User-Agent": "PolytopeEdgeDiscovery/0.1",
                },
            )
            with urllib.request.urlopen(request, timeout=60) as response:
                result = json.loads(response.read().decode("utf-8"))

        result = unwrap_pmxt_response(result)
        if isinstance(result, list):
            return result
        return [result] if result else []


def unwrap_pmxt_response(value: Any) -> Any:
    if isinstance(value, dict):
        for key in ("result", "data", "response"):
            if key in value:
                return unwrap_pmxt_response(value[key])
    return value


def fetch_chunk(
    client: PmxtClient,
    market: MarketRow,
    outcome: str,
    since_ms: int,
    until_ms: int,
    limit: int,
    retries: int,
) -> ChunkResult:
    token_id = market.yes_token_id if outcome == "yes" else market.no_token_id
    param_variants = [
        {
            "since": since_ms,
            "until": until_ms,
            "outcome": outcome,
            "limit": limit,
        },
        {
            "since": since_ms,
            "until": until_ms,
            "outcome": token_id,
            "limit": limit,
        },
    ]
    last_error = ""
    for variant_index, params in enumerate(param_variants):
        for attempt in range(retries + 1):
            try:
                books = client.fetch_order_book(market.market_id, params)
                rows = [summarize_book(book, market, outcome) for book in books]
                rows = [row for row in rows if row["timestamp"] is not None]
                rows.sort(key=lambda row: row["timestamp"])
                status = "ok" if rows else "no_rows"
                return ChunkResult(
                    rows=rows,
                    status=status,
                    error="",
                    retry_count=attempt,
                )
            except urllib.error.HTTPError as exc:
                detail = exc.read().decode("utf-8", errors="replace")
                last_error = f"http_{exc.code}: {detail[:500]}"
                if variant_index == 0 and exc.code == 400 and "Could not resolve outcome" in detail:
                    break
            except Exception as exc:  # noqa: BLE001
                last_error = f"{type(exc).__name__}: {exc}"
            if attempt < retries:
                time.sleep(min(2.0 * (attempt + 1), 10.0))
    return ChunkResult(rows=[], status="error", error=last_error, retry_count=retries)


def time_range_for_market(market: MarketRow, args: argparse.Namespace, as_of: dt.datetime) -> tuple[int, int]:
    as_of_ms = int(as_of.timestamp() * 1000)
    expiry_ms = market.expiry_ts * 1000
    until_ms = min(as_of_ms, expiry_ms) if expiry_ms > 0 else as_of_ms
    if args.lookback_days is not None:
        since_ms = until_ms - int(args.lookback_days * 24 * 60 * 60 * 1000)
    else:
        since_ms = until_ms - int(args.hours * 60 * 60 * 1000)
    return max(0, since_ms), until_ms


def parquet_path(out_dir: Path, market: MarketRow, outcome: str) -> Path:
    return out_dir / market.coin / f"{market.market_id}_{outcome}.parquet"


def write_market_outcome(
    client: PmxtClient,
    market: MarketRow,
    outcome: str,
    args: argparse.Namespace,
    as_of: dt.datetime,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    since_ms, until_ms = time_range_for_market(market, args, as_of)
    chunk_ms = args.limit * args.step_sec * 1000
    manifest_rows: list[dict[str, Any]] = []
    all_rows: list[dict[str, Any]] = []
    cursor = since_ms
    while cursor < until_ms:
        chunk_until = min(cursor + chunk_ms, until_ms)
        if args.dry_run:
            result = ChunkResult(rows=[], status="dry_run", error="", retry_count=0)
        else:
            result = fetch_chunk(client, market, outcome, cursor, chunk_until, args.limit, args.retries)
            all_rows.extend(result.rows)

        first_ts = result.rows[0]["timestamp"] if result.rows else None
        last_ts = result.rows[-1]["timestamp"] if result.rows else None
        manifest_rows.append(
            {
                "market_id": market.market_id,
                "coin": market.coin,
                "outcome": outcome,
                "since_ms": cursor,
                "until_ms": chunk_until,
                "rows_returned": len(result.rows),
                "first_ts": first_ts or "",
                "last_ts": last_ts or "",
                "status": result.status,
                "error": result.error,
                "retry_count": result.retry_count,
            }
        )
        cursor = chunk_until

    # PMXT chunks can overlap at boundaries; de-duplicate by timestamp.
    deduped = {row["timestamp"]: row for row in all_rows}
    rows = [deduped[key] for key in sorted(deduped)]
    if not args.dry_run:
        path = parquet_path(Path(args.out_dir), market, outcome)
        path.parent.mkdir(parents=True, exist_ok=True)
        pd.DataFrame(rows).to_parquet(path, index=False)
    return rows, manifest_rows


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * q
    lo = math.floor(position)
    hi = math.ceil(position)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def rate(count: int, total: int) -> float | None:
    return None if total <= 0 else count / total


def quality_for_rows(rows: list[dict[str, Any]], wide_spread_threshold: float) -> dict[str, Any]:
    total = len(rows)
    timestamps = [int(row["timestamp"]) for row in rows if row.get("timestamp") is not None]
    intervals = [
        (later - earlier) / 1000.0
        for earlier, later in zip(timestamps[:-1], timestamps[1:])
        if later >= earlier
    ]
    missing_bid = sum(1 for row in rows if row.get("best_bid") is None or pd.isna(row.get("best_bid")))
    missing_ask = sum(1 for row in rows if row.get("best_ask") is None or pd.isna(row.get("best_ask")))
    crossed = sum(
        1
        for row in rows
        if row.get("best_bid") is not None
        and row.get("best_ask") is not None
        and not pd.isna(row.get("best_bid"))
        and not pd.isna(row.get("best_ask"))
        and row["best_bid"] > row["best_ask"]
    )
    negative_spread = sum(
        1
        for row in rows
        if row.get("spread") is not None and not pd.isna(row.get("spread")) and row["spread"] < 0
    )
    wide_spread = sum(
        1
        for row in rows
        if row.get("spread") is not None and not pd.isna(row.get("spread")) and row["spread"] > wide_spread_threshold
    )
    empty_book = sum(1 for row in rows if int(row.get("num_bids") or 0) == 0 and int(row.get("num_asks") or 0) == 0)
    one_sided_book = sum(
        1
        for row in rows
        if (int(row.get("num_bids") or 0) == 0) != (int(row.get("num_asks") or 0) == 0)
    )
    empty_book_rate = rate(empty_book, total)
    one_sided_book_rate = rate(one_sided_book, total)
    quality_flag = "ok"
    if total > 0 and empty_book_rate is not None and empty_book_rate > 0.5:
        quality_flag = "low_liquidity"
    elif total > 0 and one_sided_book_rate is not None and one_sided_book_rate > 0.5:
        quality_flag = "one_sided"

    return {
        "rows": total,
        "first_ts": min(timestamps) if timestamps else None,
        "last_ts": max(timestamps) if timestamps else None,
        "coverage_hours": ((max(timestamps) - min(timestamps)) / 3_600_000.0) if len(timestamps) >= 2 else 0.0,
        "median_interval_sec": percentile(intervals, 0.50),
        "p95_interval_sec": percentile(intervals, 0.95),
        "missing_best_bid_rate": rate(missing_bid, total),
        "missing_best_ask_rate": rate(missing_ask, total),
        "crossed_book_count": crossed,
        "negative_spread_count": negative_spread,
        "wide_spread_rate": rate(wide_spread, total),
        "empty_book_rate": empty_book_rate,
        "one_sided_book_rate": one_sided_book_rate,
        "timestamp_monotonic": all(later >= earlier for earlier, later in zip(timestamps[:-1], timestamps[1:])),
        "quality_flag": quality_flag,
    }


def write_manifest(out_dir: Path, manifest_rows: list[dict[str, Any]]) -> None:
    fieldnames = [
        "market_id",
        "coin",
        "outcome",
        "since_ms",
        "until_ms",
        "rows_returned",
        "first_ts",
        "last_ts",
        "status",
        "error",
        "retry_count",
    ]
    out_dir.mkdir(parents=True, exist_ok=True)
    with (out_dir / "fetch_manifest.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(manifest_rows)


def write_quality_report(out_dir: Path, quality: dict[str, Any]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    with (out_dir / "quality_report.json").open("w", encoding="utf-8") as handle:
        json.dump(quality, handle, indent=2, sort_keys=True)
        handle.write("\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--universe", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--coins", nargs="+", default=["BTC", "ETH", "SOL"])
    parser.add_argument("--markets-per-coin", type=int, default=2)
    parser.add_argument("--hours", type=float, default=6.0)
    parser.add_argument("--lookback-days", type=float, default=None)
    parser.add_argument("--step-sec", type=int, default=5)
    parser.add_argument("--limit", type=int, default=1000)
    parser.add_argument("--retries", type=int, default=2)
    parser.add_argument("--transport", choices=["auto", "pmxt", "http"], default="auto")
    parser.add_argument("--pmxt-api-key", default=os.getenv("PMXT_API_KEY") or "")
    parser.add_argument("--wide-spread-threshold", type=float, default=0.05)
    parser.add_argument("--dry-run", action="store_true", help="Plan tasks and write reports without calling PMXT.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.limit <= 0 or args.limit > 1000:
        raise ValueError("--limit must be in [1, 1000]")
    if args.step_sec <= 0:
        raise ValueError("--step-sec must be positive")
    if not args.dry_run and not args.pmxt_api_key:
        raise RuntimeError("PMXT_API_KEY is required unless --dry-run is set")

    universe = read_universe(Path(args.universe))
    as_of = now_utc()
    selected = select_markets(universe, args.coins, args.markets_per_coin, int(as_of.timestamp()))
    out_dir = Path(args.out_dir)
    client = PmxtClient(args.pmxt_api_key, args.transport) if not args.dry_run else None

    manifest_rows: list[dict[str, Any]] = []
    quality: dict[str, Any] = {
        "generated_at": as_of.isoformat(),
        "universe": args.universe,
        "out_dir": args.out_dir,
        "transport": args.transport,
        "dry_run": args.dry_run,
        "requested": {
            "coins": args.coins,
            "markets_per_coin": args.markets_per_coin,
            "hours": args.hours,
            "lookback_days": args.lookback_days,
            "step_sec": args.step_sec,
            "limit": args.limit,
        },
        "market_outcomes": {},
    }

    for market in selected:
        for outcome in OUTCOMES:
            rows, manifest = write_market_outcome(client, market, outcome, args, as_of)
            manifest_rows.extend(manifest)
            key = f"{market.coin}/{market.market_id}/{outcome}"
            quality["market_outcomes"][key] = {
                "market_id": market.market_id,
                "slug": market.slug,
                "coin": market.coin,
                "strike": market.strike,
                "expiry_ts": market.expiry_ts,
                "outcome": outcome,
                **quality_for_rows(rows, args.wide_spread_threshold),
            }

    write_manifest(out_dir, manifest_rows)
    write_quality_report(out_dir, quality)
    print(f"selected_markets={len(selected)} tasks={len(selected) * len(OUTCOMES)} chunks={len(manifest_rows)} out_dir={out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

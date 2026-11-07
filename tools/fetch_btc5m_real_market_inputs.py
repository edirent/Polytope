#!/usr/bin/env python3
"""Fetch real BTC 5m Polymarket inputs for the C++ 30m replay tool."""

from __future__ import annotations

import argparse
import csv
import json
import math
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


GAMMA_EVENT = "https://gamma-api.polymarket.com/events/slug/{slug}"
CLOB_BOOK = "https://clob.polymarket.com/book"
CLOB_PRICE_HISTORY = "https://clob.polymarket.com/prices-history"
POLY_TRADES = "https://data-api.polymarket.com/trades"
COINBASE_CANDLES = "https://api.exchange.coinbase.com/products/BTC-USD/candles"


@dataclass
class Market:
    slug: str
    condition_id: str
    up_token: str
    down_token: str
    start_ts: int
    end_ts: int
    title: str
    gamma_best_bid: float
    gamma_best_ask: float
    gamma_last_trade: float


def get_json(url: str, params: dict[str, Any] | None = None) -> Any:
    if params:
        url = url + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "Polytope-real-market-backtest/1.0",
        },
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read().decode("utf-8"))


def parse_json_array(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    if isinstance(value, str):
        return json.loads(value)
    return []


def fetch_market(start_ts: int) -> Market:
    slug = f"btc-updown-5m-{start_ts}"
    event = get_json(GAMMA_EVENT.format(slug=slug))
    markets = event.get("markets") or []
    if not markets:
        raise RuntimeError(f"Gamma event has no market: {slug}")
    market = markets[0]
    tokens = parse_json_array(market.get("clobTokenIds"))
    if len(tokens) != 2:
        raise RuntimeError(f"Gamma market missing two CLOB tokens: {slug}")
    event_start = market.get("eventStartTime")
    end_date = market.get("endDate")
    if not event_start or not end_date:
        raise RuntimeError(f"Gamma market missing eventStartTime/endDate: {slug}")
    start = int(datetime.fromisoformat(event_start.replace("Z", "+00:00")).timestamp())
    end = int(datetime.fromisoformat(end_date.replace("Z", "+00:00")).timestamp())
    return Market(
        slug=slug,
        condition_id=str(market["conditionId"]),
        up_token=str(tokens[0]),
        down_token=str(tokens[1]),
        start_ts=start,
        end_ts=end,
        title=str(event.get("title") or market.get("question") or slug),
        gamma_best_bid=float(market.get("bestBid") or 0.0),
        gamma_best_ask=float(market.get("bestAsk") or 0.0),
        gamma_last_trade=float(market.get("lastTradePrice") or 0.0),
    )


def token_book(token_id: str) -> tuple[float, float] | None:
    try:
        book = get_json(CLOB_BOOK, {"token_id": token_id})
    except Exception:
        return None
    bids = [float(row["price"]) for row in book.get("bids") or []]
    asks = [float(row["price"]) for row in book.get("asks") or []]
    if not bids or not asks:
        return None
    return max(bids), min(asks)


def price_history(token_id: str, start_ts: int, end_ts: int) -> list[tuple[int, float]]:
    try:
        data = get_json(
            CLOB_PRICE_HISTORY,
            {
                "market": token_id,
                "startTs": start_ts,
                "endTs": end_ts,
                "fidelity": 10,
            },
        )
    except Exception:
        return []
    return sorted(
        (int(row["t"]), float(row["p"]))
        for row in data.get("history", [])
        if "t" in row and "p" in row
    )


def market_trades(condition_id: str, start_ts: int, end_ts: int) -> list[dict[str, Any]]:
    batch = get_json(POLY_TRADES, {"market": condition_id, "limit": 1000})
    if not isinstance(batch, list):
        return []
    return [
        row for row in batch
        if start_ts <= int(row.get("timestamp", 0)) < end_ts
    ]


def coinbase_candles(start_ts: int, end_ts: int) -> dict[int, tuple[float, float]]:
    data = get_json(
        COINBASE_CANDLES,
        {
            "granularity": 60,
            "start": datetime.fromtimestamp(start_ts, timezone.utc).isoformat().replace("+00:00", "Z"),
            "end": datetime.fromtimestamp(end_ts, timezone.utc).isoformat().replace("+00:00", "Z"),
        },
    )
    if not isinstance(data, list):
        raise RuntimeError(f"Coinbase candle error: {data}")
    candles: dict[int, tuple[float, float]] = {}
    for row in data:
        # Coinbase returns [time, low, high, open, close, volume].
        candles[int(row[0])] = (float(row[3]), float(row[4]))
    return candles


def interpolate_spot(candles: dict[int, tuple[float, float]], ts: int) -> float:
    minute = ts - ts % 60
    if minute not in candles:
        if candles:
            nearest = min(candles, key=lambda key: abs(key - minute))
            return candles[nearest][1]
        raise RuntimeError("no Coinbase candles available")
    open_price, close_price = candles[minute]
    frac = (ts - minute) / 59.0
    return open_price + (close_price - open_price) * max(0.0, min(1.0, frac))


def realized_sigma(candles: dict[int, tuple[float, float]]) -> float:
    closes = [candles[key][1] for key in sorted(candles)]
    returns = [
        math.log(closes[i] / closes[i - 1])
        for i in range(1, len(closes))
        if closes[i] > 0 and closes[i - 1] > 0
    ]
    if len(returns) < 2:
        return 0.55
    mean = sum(returns) / len(returns)
    variance = sum((value - mean) ** 2 for value in returns) / (len(returns) - 1)
    annualized = math.sqrt(variance) * math.sqrt(365 * 24 * 60)
    return min(2.0, max(0.15, annualized))


def tick(price: float) -> int:
    return max(1, min(999_999, round(price * 1_000_000)))


def price_at_second(
    history: list[tuple[int, float]],
    trades: list[dict[str, Any]],
    token_id: str,
    second: int,
    fallback: float,
) -> float:
    candidates = [price for ts, price in history if ts <= second]
    trade_prices = [
        float(row["price"])
        for row in trades
        if str(row.get("asset")) == token_id and int(row.get("timestamp", 0)) <= second
    ]
    if trade_prices:
        return trade_prices[-1]
    if candidates:
        return candidates[-1]
    return fallback


def trade_crosses(
    trades: list[dict[str, Any]],
    token_id: str,
    second: int,
) -> tuple[int, int]:
    buys = [
        float(row["price"])
        for row in trades
        if str(row.get("asset")) == token_id
        and int(row.get("timestamp", 0)) == second
        and str(row.get("side", "")).upper() == "BUY"
    ]
    sells = [
        float(row["price"])
        for row in trades
        if str(row.get("asset")) == token_id
        and int(row.get("timestamp", 0)) == second
        and str(row.get("side", "")).upper() == "SELL"
    ]
    return (tick(max(buys)) if buys else 0, tick(min(sells)) if sells else 0)


def write_inputs(
    markets: list[Market],
    out_dir: Path,
    source_note: str,
) -> Path:
    start_ts = min(m.start_ts for m in markets)
    end_ts = max(m.end_ts for m in markets)
    candles = coinbase_candles(start_ts, end_ts)
    sigma = realized_sigma(candles)
    metadata: dict[str, Any] = {
        "source_note": source_note,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "start_utc": datetime.fromtimestamp(start_ts, timezone.utc).isoformat(),
        "end_utc": datetime.fromtimestamp(end_ts, timezone.utc).isoformat(),
        "sigma_annualized": sigma,
        "markets": [],
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "real_inputs.csv"
    with csv_path.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow([
            "second",
            "round",
            "market_slug",
            "condition_id",
            "up_token_id",
            "down_token_id",
            "round_start_ms",
            "round_expiry_ms",
            "start_spot",
            "current_spot",
            "sigma_annualized",
            "up_bid_tick",
            "up_ask_tick",
            "down_bid_tick",
            "down_ask_tick",
            "up_buy_trade_tick",
            "up_sell_trade_tick",
            "down_buy_trade_tick",
            "down_sell_trade_tick",
        ])
        for round_index, market in enumerate(markets):
            up_book = token_book(market.up_token)
            down_book = token_book(market.down_token)
            up_history = price_history(market.up_token, market.start_ts - 300, market.end_ts)
            down_history = price_history(market.down_token, market.start_ts - 300, market.end_ts)
            trades = market_trades(market.condition_id, market.start_ts, market.end_ts)
            fallback_up = market.gamma_last_trade or 0.5
            fallback_down = 1.0 - fallback_up
            if up_book:
                fallback_up = sum(up_book) / 2.0
            if down_book:
                fallback_down = sum(down_book) / 2.0
            metadata["markets"].append({
                "slug": market.slug,
                "title": market.title,
                "condition_id": market.condition_id,
                "up_token": market.up_token,
                "down_token": market.down_token,
                "start_utc": datetime.fromtimestamp(market.start_ts, timezone.utc).isoformat(),
                "end_utc": datetime.fromtimestamp(market.end_ts, timezone.utc).isoformat(),
                "gamma_best_bid": market.gamma_best_bid,
                "gamma_best_ask": market.gamma_best_ask,
                "gamma_last_trade": market.gamma_last_trade,
                "up_price_history_points": len(up_history),
                "down_price_history_points": len(down_history),
                "trade_count": len(trades),
            })
            start_spot = interpolate_spot(candles, market.start_ts)
            for second in range(market.start_ts, market.end_ts):
                spot = interpolate_spot(candles, second)
                up_price = price_at_second(up_history, trades, market.up_token, second, fallback_up)
                down_price = price_at_second(down_history, trades, market.down_token, second, fallback_down)
                up_bid = tick(up_price - 0.005)
                up_ask = tick(up_price + 0.005)
                down_bid = tick(down_price - 0.005)
                down_ask = tick(down_price + 0.005)
                up_buy, up_sell = trade_crosses(trades, market.up_token, second)
                down_buy, down_sell = trade_crosses(trades, market.down_token, second)
                writer.writerow([
                    second,
                    round_index,
                    market.slug,
                    market.condition_id,
                    market.up_token,
                    market.down_token,
                    market.start_ts * 1000,
                    market.end_ts * 1000,
                    f"{start_spot:.8f}",
                    f"{spot:.8f}",
                    f"{sigma:.8f}",
                    up_bid,
                    up_ask,
                    down_bid,
                    down_ask,
                    up_buy,
                    up_sell,
                    down_buy,
                    down_sell,
                ])
    (out_dir / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    return csv_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--anchor-start-unix", type=int, default=1782745500)
    parser.add_argument("--out-dir", default="")
    parser.add_argument(
        "--window",
        choices=["latest-complete-before-anchor", "anchor-forward"],
        default="latest-complete-before-anchor",
    )
    args = parser.parse_args()

    now = int(time.time())
    anchor = fetch_market(args.anchor_start_unix)
    if args.window == "anchor-forward":
        replay_start = anchor.start_ts
        source_note = (
            "Anchor-forward window requested. If any seconds are in the future, "
            "public APIs cannot provide real historical BTC/Polymarket data for them."
        )
    else:
        replay_start = anchor.start_ts - 30 * 60
        source_note = (
            f"Anchor market {anchor.slug} is used as the latest reference. "
            "Because it is not a fully elapsed 30m future window at generation time, "
            "the replay uses the latest complete 30m ending at the anchor start."
        )
    replay_end = replay_start + 30 * 60
    if replay_end > now:
        raise RuntimeError(
            "Requested real replay window extends into the future: "
            f"end={datetime.fromtimestamp(replay_end, timezone.utc).isoformat()} "
            f"now={datetime.fromtimestamp(now, timezone.utc).isoformat()}"
        )

    starts = list(range(replay_start, replay_end, 300))
    markets = [fetch_market(start) for start in starts]
    out_dir = Path(args.out_dir or f"runs/btc5m_real_market_inputs_{replay_start}_{replay_end}")
    csv_path = write_inputs(markets, out_dir, source_note)
    print(f"csv={csv_path}")
    print(f"metadata={out_dir / 'metadata.json'}")
    print(f"window={datetime.fromtimestamp(replay_start, timezone.utc).isoformat()}..{datetime.fromtimestamp(replay_end, timezone.utc).isoformat()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

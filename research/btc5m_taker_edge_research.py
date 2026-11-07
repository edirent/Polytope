#!/usr/bin/env python3
"""Live BTC 5m Up/Down taker edge research.

Read-only collector. It samples the current Polymarket BTC 5m Up/Down CLOB,
Coinbase BTC-USD, Binance/Binance.US BTCUSDT, and Deribit BTC perpetual data,
then estimates terminal digital fair and taker buy edge for UP/DOWN.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import dataclasses
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import statistics
import time
from typing import Any
import urllib.error
import urllib.parse
import urllib.request


GAMMA_EVENT = "https://gamma-api.polymarket.com/events/slug/{slug}"
POLY_BOOK = "https://clob.polymarket.com/book"
COINBASE_TICKER = "https://api.exchange.coinbase.com/products/BTC-USD/ticker"
BINANCE_SPOT_BOOK_TICKER = (
    "https://api.binance.com/api/v3/ticker/bookTicker?symbol=BTCUSDT"
)
BINANCE_US_SPOT_BOOK_TICKER = (
    "https://api.binance.us/api/v3/ticker/bookTicker?symbol=BTCUSDT"
)
BINANCE_PERP_BOOK_TICKER = (
    "https://fapi.binance.com/fapi/v1/ticker/bookTicker?symbol=BTCUSDT"
)
DERIBIT_TICKER = (
    "https://www.deribit.com/api/v2/public/ticker?"
    "instrument_name=BTC-PERPETUAL"
)

SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0


@dataclasses.dataclass
class MarketInfo:
    slug: str
    title: str
    condition_id: str
    up_token: str
    down_token: str
    start_ts: int
    end_ts: int


@dataclasses.dataclass
class RoundState:
    market: MarketInfo
    strike: float = math.nan
    strike_source: str = "unset"


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_utc")


def now_iso(ts: float | None = None) -> str:
    if ts is None:
        ts = time.time()
    return datetime.fromtimestamp(ts, timezone.utc).isoformat()


def number(value: Any, default: float = math.nan) -> float:
    if value is None or value == "":
        return default
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    return out if math.isfinite(out) else default


def finite(value: float) -> bool:
    return math.isfinite(value)


def median(values: list[float]) -> float:
    clean = [v for v in values if finite(v) and v > 0]
    return statistics.median(clean) if clean else math.nan


def percentile(values: list[float], q: float) -> float:
    clean = sorted(v for v in values if finite(v))
    if not clean:
        return math.nan
    idx = min(len(clean) - 1, max(0, round((len(clean) - 1) * q)))
    return clean[idx]


def mean(values: list[float]) -> float:
    clean = [v for v in values if finite(v)]
    return statistics.fmean(clean) if clean else math.nan


def http_json(
    url: str,
    params: dict[str, Any] | None = None,
    timeout_s: float = 4.0,
) -> Any:
    if params:
        url = url + "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "PolytopeBTC5mTakerEdgeResearch/0.1",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return json.loads(response.read().decode("utf-8"))


def parse_json_array(value: Any) -> list[Any]:
    if isinstance(value, list):
        return value
    if isinstance(value, str) and value:
        try:
            parsed = json.loads(value)
        except json.JSONDecodeError:
            return []
        return parsed if isinstance(parsed, list) else []
    return []


def fetch_market(start_ts: int) -> MarketInfo:
    slug = f"btc-updown-5m-{start_ts}"
    event = http_json(GAMMA_EVENT.format(slug=slug), timeout_s=8.0)
    markets = event.get("markets") or []
    if not markets:
        raise RuntimeError(f"missing Gamma market for {slug}")
    market = markets[0]
    tokens = [str(x) for x in parse_json_array(market.get("clobTokenIds"))]
    if len(tokens) < 2:
        raise RuntimeError(f"missing UP/DOWN token IDs for {slug}")
    event_start = market.get("eventStartTime")
    event_end = market.get("endDate")
    if not event_start or not event_end:
        raise RuntimeError(f"missing eventStartTime/endDate for {slug}")
    start = int(datetime.fromisoformat(event_start.replace("Z", "+00:00")).timestamp())
    end = int(datetime.fromisoformat(event_end.replace("Z", "+00:00")).timestamp())
    return MarketInfo(
        slug=slug,
        title=str(event.get("title") or market.get("question") or slug),
        condition_id=str(market.get("conditionId") or ""),
        up_token=tokens[0],
        down_token=tokens[1],
        start_ts=start,
        end_ts=end,
    )


def sorted_levels(rows: Any, side: str) -> list[tuple[float, float]]:
    levels: list[tuple[float, float]] = []
    if not isinstance(rows, list):
        return levels
    for row in rows:
        if not isinstance(row, dict):
            continue
        price = number(row.get("price"))
        size = number(row.get("size"))
        if finite(price) and finite(size) and price > 0 and size > 0:
            levels.append((price, size))
    return sorted(levels, key=lambda item: item[0], reverse=(side == "bid"))


def fetch_poly_book(token_id: str) -> dict[str, Any]:
    book = http_json(POLY_BOOK, {"token_id": token_id}, timeout_s=4.0)
    bids = sorted_levels(book.get("bids"), "bid")
    asks = sorted_levels(book.get("asks"), "ask")
    best_bid, best_bid_size = bids[0] if bids else (math.nan, math.nan)
    best_ask, best_ask_size = asks[0] if asks else (math.nan, math.nan)
    return {
        "best_bid": best_bid,
        "best_bid_size": best_bid_size,
        "best_ask": best_ask,
        "best_ask_size": best_ask_size,
        "mid": (best_bid + best_ask) / 2.0 if finite(best_bid) and finite(best_ask) else math.nan,
        "spread": best_ask - best_bid if finite(best_bid) and finite(best_ask) else math.nan,
        "bid_levels": len(bids),
        "ask_levels": len(asks),
        "bid_depth_top5": sum(size for _, size in bids[:5]),
        "ask_depth_top5": sum(size for _, size in asks[:5]),
        "asks": asks,
        "bids": bids,
    }


def vwap_buy(asks: list[tuple[float, float]], qty: float) -> tuple[float, float, float]:
    if qty <= 0:
        return math.nan, 0.0, math.nan
    remaining = qty
    notional = 0.0
    filled = 0.0
    worst = math.nan
    for price, size in asks:
        take = min(remaining, size)
        if take <= 0:
            continue
        notional += take * price
        filled += take
        remaining -= take
        worst = price
        if remaining <= 1e-12:
            break
    if filled + 1e-12 < qty:
        return math.nan, filled, worst
    return notional / qty, filled, worst


def fetch_coinbase() -> dict[str, float | str]:
    data = http_json(COINBASE_TICKER, timeout_s=4.0)
    bid = number(data.get("bid"))
    ask = number(data.get("ask"))
    price = number(data.get("price"))
    mid = (bid + ask) / 2.0 if finite(bid) and finite(ask) else price
    return {
        "status": "ok",
        "bid": bid,
        "ask": ask,
        "price": price,
        "mid": mid,
    }


def parse_book_ticker(data: Any) -> dict[str, float | str]:
    bid = number(data.get("bidPrice")) if isinstance(data, dict) else math.nan
    ask = number(data.get("askPrice")) if isinstance(data, dict) else math.nan
    mid = (bid + ask) / 2.0 if finite(bid) and finite(ask) else math.nan
    return {
        "status": "ok",
        "bid": bid,
        "ask": ask,
        "mid": mid,
    }


def fetch_binance_spot() -> dict[str, float | str]:
    try:
        return parse_book_ticker(http_json(BINANCE_SPOT_BOOK_TICKER, timeout_s=3.0))
    except Exception as exc:
        try:
            out = parse_book_ticker(http_json(BINANCE_US_SPOT_BOOK_TICKER, timeout_s=3.0))
            out["status"] = "ok_binance_us_fallback"
            return out
        except Exception:
            return {
                "status": f"error:{type(exc).__name__}",
                "bid": math.nan,
                "ask": math.nan,
                "mid": math.nan,
            }


def fetch_binance_perp() -> dict[str, float | str]:
    try:
        return parse_book_ticker(http_json(BINANCE_PERP_BOOK_TICKER, timeout_s=3.0))
    except Exception as exc:
        return {
            "status": f"error:{type(exc).__name__}",
            "bid": math.nan,
            "ask": math.nan,
            "mid": math.nan,
        }


def fetch_deribit_perp() -> dict[str, float | str]:
    data = http_json(DERIBIT_TICKER, timeout_s=4.0)
    result = data.get("result") if isinstance(data, dict) else {}
    bid = number(result.get("best_bid_price"))
    ask = number(result.get("best_ask_price"))
    mark = number(result.get("mark_price"))
    index = number(result.get("index_price"))
    mid = (bid + ask) / 2.0 if finite(bid) and finite(ask) else mark
    return {
        "status": "ok",
        "bid": bid,
        "ask": ask,
        "mid": mid,
        "mark": mark,
        "index": index,
    }


def norm_cdf(value: float) -> float:
    return 0.5 * math.erfc(-value / math.sqrt(2.0))


def fair_up_probability(
    spot: float,
    strike: float,
    sigma_annualized: float,
    tau_sec: float,
) -> float:
    if not (spot > 0 and strike > 0 and sigma_annualized > 0):
        return math.nan
    if tau_sec <= 0:
        return 1.0 if spot > strike else 0.0
    tau_years = tau_sec / SECONDS_PER_YEAR
    denom = sigma_annualized * math.sqrt(tau_years)
    if denom <= 0 or not finite(denom):
        return math.nan
    d2 = (
        math.log(spot / strike) - 0.5 * sigma_annualized * sigma_annualized * tau_years
    ) / denom
    return max(0.0, min(1.0, norm_cdf(d2)))


def realized_vol_annualized(samples: list[tuple[float, float]], default: float) -> float:
    clean = [(ts, spot) for ts, spot in samples if spot > 0 and finite(spot)]
    if len(clean) < 6:
        return default
    returns: list[float] = []
    deltas: list[float] = []
    for (prev_ts, prev_spot), (ts, spot) in zip(clean, clean[1:]):
        dt = ts - prev_ts
        if dt <= 0:
            continue
        returns.append(math.log(spot / prev_spot))
        deltas.append(dt)
    if len(returns) < 5:
        return default
    stdev = statistics.stdev(returns)
    avg_dt = statistics.fmean(deltas) if deltas else 1.0
    if avg_dt <= 0:
        return default
    return max(0.01, min(5.0, stdev * math.sqrt(SECONDS_PER_YEAR / avg_dt)))


def fmt(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return "" if not finite(value) else f"{value:.10f}"
    if value is None:
        return ""
    return str(value)


def source_ok(status: Any) -> int:
    return 1 if isinstance(status, str) and status.startswith("ok") else 0


CSV_FIELDS = [
    "sample_index",
    "recv_ts",
    "recv_utc",
    "market_slug",
    "condition_id",
    "up_token",
    "down_token",
    "round_start_ts",
    "round_end_ts",
    "round_start_price_k",
    "k_source",
    "tau_sec",
    "coinbase_bid",
    "coinbase_ask",
    "coinbase_mid",
    "binance_spot_status",
    "binance_spot_bid",
    "binance_spot_ask",
    "binance_spot_mid",
    "binance_perp_status",
    "binance_perp_bid",
    "binance_perp_ask",
    "binance_perp_mid",
    "deribit_perp_bid",
    "deribit_perp_ask",
    "deribit_perp_mid",
    "deribit_mark",
    "deribit_index",
    "spot_index_mid",
    "sigma_annualized",
    "fair_up",
    "up_best_bid",
    "up_best_bid_size",
    "up_best_ask",
    "up_best_ask_size",
    "up_mid",
    "up_spread",
    "up_bid_levels",
    "up_ask_levels",
    "up_bid_depth_top5",
    "up_ask_depth_top5",
    "down_best_bid",
    "down_best_bid_size",
    "down_best_ask",
    "down_best_ask_size",
    "down_mid",
    "down_spread",
    "down_bid_levels",
    "down_ask_levels",
    "down_bid_depth_top5",
    "down_ask_depth_top5",
    "taker_qty",
    "vwap_buy_up",
    "vwap_buy_up_filled_qty",
    "vwap_buy_up_worst_price",
    "vwap_buy_down",
    "vwap_buy_down_filled_qty",
    "vwap_buy_down_worst_price",
    "edge_buy_up",
    "edge_buy_down",
    "edge_buy_up_tick",
    "edge_buy_down_tick",
    "source_ok_coinbase",
    "source_ok_binance_spot",
    "source_ok_binance_perp",
    "source_ok_deribit",
    "source_ok_poly_up",
    "source_ok_poly_down",
]


def row_from_sample(
    sample_index: int,
    recv_ts: float,
    state: RoundState,
    up_book: dict[str, Any],
    down_book: dict[str, Any],
    coinbase: dict[str, Any],
    binance_spot: dict[str, Any],
    binance_perp: dict[str, Any],
    deribit: dict[str, Any],
    spot_index_mid: float,
    sigma: float,
    fair_up: float,
    qty: float,
    vwap_up: tuple[float, float, float],
    vwap_down: tuple[float, float, float],
) -> dict[str, Any]:
    edge_up = fair_up - vwap_up[0] if finite(fair_up) and finite(vwap_up[0]) else math.nan
    fair_down = 1.0 - fair_up if finite(fair_up) else math.nan
    edge_down = fair_down - vwap_down[0] if finite(fair_down) and finite(vwap_down[0]) else math.nan
    market = state.market
    return {
        "sample_index": sample_index,
        "recv_ts": recv_ts,
        "recv_utc": now_iso(recv_ts),
        "market_slug": market.slug,
        "condition_id": market.condition_id,
        "up_token": market.up_token,
        "down_token": market.down_token,
        "round_start_ts": market.start_ts,
        "round_end_ts": market.end_ts,
        "round_start_price_k": state.strike,
        "k_source": state.strike_source,
        "tau_sec": max(0.0, market.end_ts - recv_ts),
        "coinbase_bid": coinbase.get("bid", math.nan),
        "coinbase_ask": coinbase.get("ask", math.nan),
        "coinbase_mid": coinbase.get("mid", math.nan),
        "binance_spot_status": binance_spot.get("status", ""),
        "binance_spot_bid": binance_spot.get("bid", math.nan),
        "binance_spot_ask": binance_spot.get("ask", math.nan),
        "binance_spot_mid": binance_spot.get("mid", math.nan),
        "binance_perp_status": binance_perp.get("status", ""),
        "binance_perp_bid": binance_perp.get("bid", math.nan),
        "binance_perp_ask": binance_perp.get("ask", math.nan),
        "binance_perp_mid": binance_perp.get("mid", math.nan),
        "deribit_perp_bid": deribit.get("bid", math.nan),
        "deribit_perp_ask": deribit.get("ask", math.nan),
        "deribit_perp_mid": deribit.get("mid", math.nan),
        "deribit_mark": deribit.get("mark", math.nan),
        "deribit_index": deribit.get("index", math.nan),
        "spot_index_mid": spot_index_mid,
        "sigma_annualized": sigma,
        "fair_up": fair_up,
        "up_best_bid": up_book["best_bid"],
        "up_best_bid_size": up_book["best_bid_size"],
        "up_best_ask": up_book["best_ask"],
        "up_best_ask_size": up_book["best_ask_size"],
        "up_mid": up_book["mid"],
        "up_spread": up_book["spread"],
        "up_bid_levels": up_book["bid_levels"],
        "up_ask_levels": up_book["ask_levels"],
        "up_bid_depth_top5": up_book["bid_depth_top5"],
        "up_ask_depth_top5": up_book["ask_depth_top5"],
        "down_best_bid": down_book["best_bid"],
        "down_best_bid_size": down_book["best_bid_size"],
        "down_best_ask": down_book["best_ask"],
        "down_best_ask_size": down_book["best_ask_size"],
        "down_mid": down_book["mid"],
        "down_spread": down_book["spread"],
        "down_bid_levels": down_book["bid_levels"],
        "down_ask_levels": down_book["ask_levels"],
        "down_bid_depth_top5": down_book["bid_depth_top5"],
        "down_ask_depth_top5": down_book["ask_depth_top5"],
        "taker_qty": qty,
        "vwap_buy_up": vwap_up[0],
        "vwap_buy_up_filled_qty": vwap_up[1],
        "vwap_buy_up_worst_price": vwap_up[2],
        "vwap_buy_down": vwap_down[0],
        "vwap_buy_down_filled_qty": vwap_down[1],
        "vwap_buy_down_worst_price": vwap_down[2],
        "edge_buy_up": edge_up,
        "edge_buy_down": edge_down,
        "edge_buy_up_tick": round(edge_up * 1_000_000) if finite(edge_up) else math.nan,
        "edge_buy_down_tick": round(edge_down * 1_000_000) if finite(edge_down) else math.nan,
        "source_ok_coinbase": source_ok(coinbase.get("status", "ok")),
        "source_ok_binance_spot": source_ok(binance_spot.get("status", "")),
        "source_ok_binance_perp": source_ok(binance_perp.get("status", "")),
        "source_ok_deribit": source_ok(deribit.get("status", "ok")),
        "source_ok_poly_up": 1 if finite(up_book["best_ask"]) else 0,
        "source_ok_poly_down": 1 if finite(down_book["best_ask"]) else 0,
    }


def summarize_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "rows": len(rows),
        "markets": sorted({row["market_slug"] for row in rows}),
        "first_utc": rows[0]["recv_utc"] if rows else "",
        "last_utc": rows[-1]["recv_utc"] if rows else "",
        "edge_buy_up": summarize_series([number(row["edge_buy_up"]) for row in rows]),
        "edge_buy_down": summarize_series([number(row["edge_buy_down"]) for row in rows]),
        "fair_up": summarize_series([number(row["fair_up"]) for row in rows]),
        "up_spread": summarize_series([number(row["up_spread"]) for row in rows]),
        "down_spread": summarize_series([number(row["down_spread"]) for row in rows]),
        "sigma_annualized": summarize_series([number(row["sigma_annualized"]) for row in rows]),
        "source_coverage": {
            "coinbase": sum(int(row["source_ok_coinbase"]) for row in rows),
            "binance_spot": sum(int(row["source_ok_binance_spot"]) for row in rows),
            "binance_perp": sum(int(row["source_ok_binance_perp"]) for row in rows),
            "deribit": sum(int(row["source_ok_deribit"]) for row in rows),
            "poly_up": sum(int(row["source_ok_poly_up"]) for row in rows),
            "poly_down": sum(int(row["source_ok_poly_down"]) for row in rows),
        },
        "positive_edge_counts": {
            "buy_up": sum(1 for row in rows if number(row["edge_buy_up"]) > 0),
            "buy_down": sum(1 for row in rows if number(row["edge_buy_down"]) > 0),
        },
        "top_buy_up": top_edges(rows, "edge_buy_up"),
        "top_buy_down": top_edges(rows, "edge_buy_down"),
    }


def summarize_series(values: list[float]) -> dict[str, Any]:
    clean = [v for v in values if finite(v)]
    if not clean:
        return {"count": 0}
    return {
        "count": len(clean),
        "min": min(clean),
        "p50": percentile(clean, 0.50),
        "p95": percentile(clean, 0.95),
        "max": max(clean),
        "mean": mean(clean),
    }


def top_edges(rows: list[dict[str, Any]], edge_field: str, limit: int = 10) -> list[dict[str, Any]]:
    ranked = sorted(
        (row for row in rows if finite(number(row.get(edge_field)))),
        key=lambda row: number(row[edge_field]),
        reverse=True,
    )
    out: list[dict[str, Any]] = []
    for row in ranked[:limit]:
        out.append({
            "recv_utc": row["recv_utc"],
            "market_slug": row["market_slug"],
            "tau_sec": number(row["tau_sec"]),
            "K": number(row["round_start_price_k"]),
            "spot": number(row["spot_index_mid"]),
            "sigma": number(row["sigma_annualized"]),
            "fair_up": number(row["fair_up"]),
            "vwap_buy_up": number(row["vwap_buy_up"]),
            "vwap_buy_down": number(row["vwap_buy_down"]),
            edge_field: number(row[edge_field]),
        })
    return out


def write_markdown_report(path: Path, summary: dict[str, Any], metadata: dict[str, Any]) -> None:
    def series_line(name: str, obj: dict[str, Any]) -> str:
        if obj.get("count", 0) == 0:
            return f"- {name}: no valid samples"
        return (
            f"- {name}: n={obj['count']} min={obj['min']:.6f} "
            f"p50={obj['p50']:.6f} p95={obj['p95']:.6f} "
            f"max={obj['max']:.6f} mean={obj['mean']:.6f}"
        )

    lines = [
        "# BTC 5m Taker Edge Research",
        "",
        f"- generated_at_utc: {metadata['generated_at_utc']}",
        f"- duration_sec: {metadata['duration_sec']}",
        f"- interval_ms: {metadata['interval_ms']}",
        f"- taker_qty: {metadata['taker_qty']}",
        f"- rows: {summary['rows']}",
        f"- first_utc: {summary['first_utc']}",
        f"- last_utc: {summary['last_utc']}",
        f"- markets: {', '.join(summary['markets'])}",
        "",
        "## Source Coverage",
    ]
    for key, value in summary["source_coverage"].items():
        lines.append(f"- {key}: {value}/{summary['rows']}")
    lines.extend([
        "",
        "## Edge Summary",
        series_line("edge_buy_up", summary["edge_buy_up"]),
        series_line("edge_buy_down", summary["edge_buy_down"]),
        series_line("fair_up", summary["fair_up"]),
        series_line("sigma_annualized", summary["sigma_annualized"]),
        series_line("up_spread", summary["up_spread"]),
        series_line("down_spread", summary["down_spread"]),
        "",
        "## Positive Edge Counts",
        f"- buy_up: {summary['positive_edge_counts']['buy_up']}/{summary['rows']}",
        f"- buy_down: {summary['positive_edge_counts']['buy_down']}/{summary['rows']}",
        "",
        "## Top Buy UP Edges",
    ])
    for item in summary["top_buy_up"]:
        lines.append(
            "- {recv_utc} tau={tau_sec:.1f} fair_up={fair_up:.4f} "
            "vwap_up={vwap_buy_up:.4f} edge={edge_buy_up:.4f} "
            "spot={spot:.2f} K={K:.2f}".format(**item)
        )
    lines.append("")
    lines.append("## Top Buy DOWN Edges")
    for item in summary["top_buy_down"]:
        lines.append(
            "- {recv_utc} tau={tau_sec:.1f} fair_up={fair_up:.4f} "
            "vwap_down={vwap_buy_down:.4f} edge={edge_buy_down:.4f} "
            "spot={spot:.2f} K={K:.2f}".format(**item)
        )
    lines.extend([
        "",
        "## Notes",
        "- fair_up uses the same lognormal digital d2 formula as the engine with zero drift.",
        "- K is the first valid composite spot observed for each round unless the data source later supplies a better round-start price.",
        "- Binance global endpoints may return HTTP 451 from this location; Binance.US spot and Deribit perp are recorded as fallbacks.",
    ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration-sec", type=int, default=300)
    parser.add_argument("--interval-ms", type=int, default=1000)
    parser.add_argument("--taker-qty", type=float, default=5.0)
    parser.add_argument("--vol-window-sec", type=int, default=60)
    parser.add_argument("--default-sigma", type=float, default=0.65)
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--align-next-round", action="store_true")
    args = parser.parse_args()

    if args.interval_ms <= 0:
        raise ValueError("--interval-ms must be positive")
    if args.taker_qty <= 0:
        raise ValueError("--taker-qty must be positive")

    if args.align_next_round:
        next_start = int(time.time()) // 300 * 300 + 300
        sleep_for = max(0.0, next_start - time.time())
        print(f"waiting_for_next_round_sec={sleep_for:.3f}", flush=True)
        time.sleep(sleep_for)

    out_dir = Path(args.out_dir or f"research/runs/btc5m_taker_edge_{utc_stamp()}")
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "samples.csv"
    summary_path = out_dir / "summary.json"
    report_path = out_dir / "report.md"

    market_cache: dict[int, RoundState] = {}
    rows: list[dict[str, Any]] = []
    spot_window: list[tuple[float, float]] = []
    errors: dict[str, int] = {}

    def fetch_safe(name: str, fn: Any) -> Any:
        try:
            return fn()
        except Exception as exc:
            errors[name] = errors.get(name, 0) + 1
            return {"status": f"error:{type(exc).__name__}"}

    started = time.time()
    end_time = started + args.duration_sec
    sample_index = 0
    print(f"out_dir={out_dir}", flush=True)

    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        writer.writeheader()
        while time.time() < end_time:
            loop_start = time.time()
            recv_ts = time.time()
            market_start = int(recv_ts) // 300 * 300
            if market_start not in market_cache:
                market = fetch_market(market_start)
                market_cache[market_start] = RoundState(market=market)
                print(
                    f"market={market.slug} title={market.title} "
                    f"up={market.up_token[:18]} down={market.down_token[:18]}",
                    flush=True,
                )
            state = market_cache[market_start]

            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                futures = {
                    "up": pool.submit(fetch_safe, "poly_up", lambda: fetch_poly_book(state.market.up_token)),
                    "down": pool.submit(fetch_safe, "poly_down", lambda: fetch_poly_book(state.market.down_token)),
                    "coinbase": pool.submit(fetch_safe, "coinbase", fetch_coinbase),
                    "binance_spot": pool.submit(fetch_binance_spot),
                    "binance_perp": pool.submit(fetch_binance_perp),
                    "deribit": pool.submit(fetch_safe, "deribit", fetch_deribit_perp),
                }
                up_book = futures["up"].result()
                down_book = futures["down"].result()
                coinbase = futures["coinbase"].result()
                binance_spot = futures["binance_spot"].result()
                binance_perp = futures["binance_perp"].result()
                deribit = futures["deribit"].result()

            for name, book in (("up", up_book), ("down", down_book)):
                if "best_ask" not in book:
                    book.update({
                        "best_bid": math.nan,
                        "best_bid_size": math.nan,
                        "best_ask": math.nan,
                        "best_ask_size": math.nan,
                        "mid": math.nan,
                        "spread": math.nan,
                        "bid_levels": 0,
                        "ask_levels": 0,
                        "bid_depth_top5": 0.0,
                        "ask_depth_top5": 0.0,
                        "asks": [],
                        "bids": [],
                    })
                    errors[f"poly_{name}"] = errors.get(f"poly_{name}", 0) + 1

            spot_index_mid = median([
                number(coinbase.get("mid")),
                number(binance_spot.get("mid")),
                number(binance_perp.get("mid")),
                number(deribit.get("mid")),
            ])
            if finite(spot_index_mid):
                spot_window.append((recv_ts, spot_index_mid))
                cutoff = recv_ts - args.vol_window_sec
                spot_window = [(ts, spot) for ts, spot in spot_window if ts >= cutoff]
                if not finite(state.strike):
                    state.strike = spot_index_mid
                    lateness = recv_ts - state.market.start_ts
                    state.strike_source = (
                        "first_sample_spot_at_round_start"
                        if lateness <= 3.0
                        else f"first_sample_spot_late_by_{lateness:.1f}s"
                    )

            tau_sec = max(0.0, state.market.end_ts - recv_ts)
            sigma = realized_vol_annualized(spot_window, args.default_sigma)
            fair_up = fair_up_probability(spot_index_mid, state.strike, sigma, tau_sec)
            vwap_up = vwap_buy(up_book["asks"], args.taker_qty)
            vwap_down = vwap_buy(down_book["asks"], args.taker_qty)
            row = row_from_sample(
                sample_index,
                recv_ts,
                state,
                up_book,
                down_book,
                coinbase,
                binance_spot,
                binance_perp,
                deribit,
                spot_index_mid,
                sigma,
                fair_up,
                args.taker_qty,
                vwap_up,
                vwap_down,
            )
            writer.writerow({key: fmt(row.get(key)) for key in CSV_FIELDS})
            fh.flush()
            rows.append(row)

            sample_index += 1
            if sample_index % max(1, round(30_000 / args.interval_ms)) == 0:
                remaining = max(0, int(end_time - time.time()))
                print(
                    f"progress samples={sample_index} remaining={remaining}s "
                    f"edge_up={fmt(row['edge_buy_up'])} edge_down={fmt(row['edge_buy_down'])}",
                    flush=True,
                )
            sleep_for = args.interval_ms / 1000.0 - (time.time() - loop_start)
            if sleep_for > 0:
                time.sleep(sleep_for)

    metadata = {
        "generated_at_utc": now_iso(),
        "duration_sec": args.duration_sec,
        "interval_ms": args.interval_ms,
        "taker_qty": args.taker_qty,
        "vol_window_sec": args.vol_window_sec,
        "default_sigma": args.default_sigma,
        "csv": str(csv_path),
        "summary": str(summary_path),
        "report": str(report_path),
        "endpoints": {
            "polymarket_gamma_event": GAMMA_EVENT,
            "polymarket_clob_book": POLY_BOOK,
            "coinbase_ticker": COINBASE_TICKER,
            "binance_spot_book_ticker": BINANCE_SPOT_BOOK_TICKER,
            "binance_us_spot_book_ticker": BINANCE_US_SPOT_BOOK_TICKER,
            "binance_perp_book_ticker": BINANCE_PERP_BOOK_TICKER,
            "deribit_ticker": DERIBIT_TICKER,
        },
        "errors": errors,
        "markets": [dataclasses.asdict(state.market) | {
            "strike": state.strike,
            "strike_source": state.strike_source,
        } for state in market_cache.values()],
    }
    summary = summarize_rows(rows)
    full_summary = {
        "metadata": metadata,
        "summary": summary,
    }
    summary_path.write_text(json.dumps(full_summary, indent=2), encoding="utf-8")
    write_markdown_report(report_path, summary, metadata)
    print(f"csv={csv_path}", flush=True)
    print(f"summary={summary_path}", flush=True)
    print(f"report={report_path}", flush=True)
    print(f"rows={len(rows)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

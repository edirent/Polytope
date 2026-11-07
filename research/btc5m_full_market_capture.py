#!/usr/bin/env python3
"""Capture complete BTC 5m Up/Down markets and build taker edge diagnostics.

This is a read-only live collector. It waits for a fresh 5m market boundary,
samples every interval for N complete markets, stores raw Polymarket CLOB
snapshots, then computes forward markouts, settlement PnL, and the four
diagnostic tables used for taker alpha review.
"""

from __future__ import annotations

import argparse
import bisect
import concurrent.futures
import csv
import dataclasses
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import statistics
import time
from typing import Any, Callable

from btc5m_taker_edge_research import (
    DERIBIT_TICKER,
    BINANCE_PERP_BOOK_TICKER,
    BINANCE_SPOT_BOOK_TICKER,
    BINANCE_US_SPOT_BOOK_TICKER,
    COINBASE_TICKER,
    GAMMA_EVENT,
    POLY_BOOK,
    MarketInfo,
    RoundState,
    fair_up_probability,
    fetch_binance_perp,
    fetch_binance_spot,
    fetch_coinbase,
    fetch_deribit_perp,
    fetch_market,
    fetch_poly_book,
    finite,
    fmt,
    http_json,
    median,
    mean,
    now_iso,
    number,
    percentile,
    realized_vol_annualized,
    source_ok,
    utc_stamp,
    vwap_buy,
)


EDGE_THRESHOLD = 0.03
FORWARD_HORIZONS_SEC = (1, 3, 5)
MOMENTUM_HORIZONS_SEC = (1, 3, 5)
TAU_BUCKETS = (
    ("tau_240_300", 240.0, 300.000001),
    ("tau_180_240", 180.0, 240.0),
    ("tau_120_180", 120.0, 180.0),
    ("tau_60_120", 60.0, 120.0),
    ("tau_30_60", 30.0, 60.0),
    ("tau_lt_30", -0.000001, 30.0),
)


BASE_FIELDS = [
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
    "sigma_floor_annualized",
    "fair_up",
    "fair_up_sigma_floor",
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
    "source_ok_coinbase",
    "source_ok_binance_spot",
    "source_ok_binance_perp",
    "source_ok_deribit",
    "source_ok_poly_up",
    "source_ok_poly_down",
]

DERIVED_FIELDS = [
    "fair_delta_1s",
    "fair_delta_3s",
    "fair_sigma_floor_delta_1s",
    "fair_sigma_floor_delta_3s",
    "spot_return_1s",
    "spot_return_3s",
    "spot_return_5s",
    "up_mid_delta_1s",
    "up_mid_delta_3s",
    "down_mid_delta_1s",
    "down_mid_delta_3s",
    "tau_bucket",
    "settlement_up",
    "settlement_down",
    "settlement_winner",
    "settlement_source",
]


def qty_label(qty: float) -> str:
    text = f"{qty:g}".replace(".", "p")
    return f"q{text}"


def qty_fields(qtys: list[float]) -> list[str]:
    fields: list[str] = []
    for qty in qtys:
        q = qty_label(qty)
        fields.extend([
            f"vwap_buy_up_{q}",
            f"vwap_buy_up_filled_qty_{q}",
            f"vwap_buy_up_worst_price_{q}",
            f"vwap_buy_down_{q}",
            f"vwap_buy_down_filled_qty_{q}",
            f"vwap_buy_down_worst_price_{q}",
            f"edge_buy_up_{q}",
            f"edge_buy_down_{q}",
            f"edge_buy_up_sigma_floor_{q}",
            f"edge_buy_down_sigma_floor_{q}",
            f"buy_up_markout_1s_{q}",
            f"buy_up_markout_3s_{q}",
            f"buy_up_markout_5s_{q}",
            f"buy_down_markout_1s_{q}",
            f"buy_down_markout_3s_{q}",
            f"buy_down_markout_5s_{q}",
            f"buy_up_settlement_pnl_{q}",
            f"buy_down_settlement_pnl_{q}",
        ])
    return fields


def csv_fields(qtys: list[float], enriched: bool = False) -> list[str]:
    fields = list(BASE_FIELDS)
    if enriched:
        fields.extend(DERIVED_FIELDS)
    fields.extend(qty_fields(qtys))
    return fields


def sanitize_json(value: Any) -> Any:
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {str(k): sanitize_json(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [sanitize_json(v) for v in value]
    return value


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


def next_round_start(now_ts: float | None = None) -> int:
    if now_ts is None:
        now_ts = time.time()
    return int(now_ts) // 300 * 300 + 300


def sleep_until(target_ts: float) -> None:
    while True:
        remaining = target_ts - time.time()
        if remaining <= 0:
            return
        time.sleep(min(remaining, 1.0))


def fetch_safe(
    errors: dict[str, int],
    name: str,
    fn: Callable[[], Any],
) -> Any:
    try:
        return fn()
    except Exception as exc:
        errors[name] = errors.get(name, 0) + 1
        return {"status": f"error:{type(exc).__name__}"}


def normalize_book(book: Any, errors: dict[str, int], name: str) -> dict[str, Any]:
    if not isinstance(book, dict) or "best_ask" not in book:
        errors[name] = errors.get(name, 0) + 1
        return {
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
        }
    return book


def spot_index_mid_from_sources(
    coinbase: dict[str, Any],
    binance_spot: dict[str, Any],
    binance_perp: dict[str, Any],
    deribit: dict[str, Any],
) -> float:
    return median([
        number(coinbase.get("mid")),
        number(binance_spot.get("mid")),
        number(binance_perp.get("mid")),
        number(deribit.get("mid")),
    ])


def row_from_live_sample(
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
    sigma_floor: float,
    fair_up: float,
    fair_up_floor: float,
    qtys: list[float],
) -> dict[str, Any]:
    market = state.market
    row: dict[str, Any] = {
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
        "sigma_floor_annualized": sigma_floor,
        "fair_up": fair_up,
        "fair_up_sigma_floor": fair_up_floor,
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
        "source_ok_coinbase": source_ok(coinbase.get("status", "ok")),
        "source_ok_binance_spot": source_ok(binance_spot.get("status", "")),
        "source_ok_binance_perp": source_ok(binance_perp.get("status", "")),
        "source_ok_deribit": source_ok(deribit.get("status", "ok")),
        "source_ok_poly_up": 1 if finite(up_book["best_ask"]) else 0,
        "source_ok_poly_down": 1 if finite(down_book["best_ask"]) else 0,
    }
    for qty in qtys:
        q = qty_label(qty)
        vwap_up = vwap_buy(up_book["asks"], qty)
        vwap_down = vwap_buy(down_book["asks"], qty)
        fair_down = 1.0 - fair_up if finite(fair_up) else math.nan
        fair_down_floor = 1.0 - fair_up_floor if finite(fair_up_floor) else math.nan
        row[f"vwap_buy_up_{q}"] = vwap_up[0]
        row[f"vwap_buy_up_filled_qty_{q}"] = vwap_up[1]
        row[f"vwap_buy_up_worst_price_{q}"] = vwap_up[2]
        row[f"vwap_buy_down_{q}"] = vwap_down[0]
        row[f"vwap_buy_down_filled_qty_{q}"] = vwap_down[1]
        row[f"vwap_buy_down_worst_price_{q}"] = vwap_down[2]
        row[f"edge_buy_up_{q}"] = fair_up - vwap_up[0] if finite(fair_up) and finite(vwap_up[0]) else math.nan
        row[f"edge_buy_down_{q}"] = fair_down - vwap_down[0] if finite(fair_down) and finite(vwap_down[0]) else math.nan
        row[f"edge_buy_up_sigma_floor_{q}"] = (
            fair_up_floor - vwap_up[0]
            if finite(fair_up_floor) and finite(vwap_up[0])
            else math.nan
        )
        row[f"edge_buy_down_sigma_floor_{q}"] = (
            fair_down_floor - vwap_down[0]
            if finite(fair_down_floor) and finite(vwap_down[0])
            else math.nan
        )
    return row


def write_csv_row(writer: csv.DictWriter, fields: list[str], row: dict[str, Any]) -> None:
    writer.writerow({key: fmt(row.get(key)) for key in fields})


def write_jsonl_row(fh: Any, obj: dict[str, Any]) -> None:
    fh.write(json.dumps(sanitize_json(obj), separators=(",", ":")) + "\n")
    fh.flush()


def fetch_settlement_once(market: MarketInfo) -> dict[str, Any]:
    event = http_json(GAMMA_EVENT.format(slug=market.slug), timeout_s=8.0)
    markets = event.get("markets") or []
    if not markets:
        return {"source": "gamma_missing_market", "up": math.nan, "down": math.nan}
    raw_market = markets[0]
    outcome_prices = [number(x) for x in parse_json_array(raw_market.get("outcomePrices"))]
    if len(outcome_prices) >= 2 and finite(outcome_prices[0]) and finite(outcome_prices[1]):
        up = outcome_prices[0]
        down = outcome_prices[1]
        if (up >= 0.99 and down <= 0.01) or (down >= 0.99 and up <= 0.01):
            return {
                "source": "gamma_outcome_prices",
                "up": 1.0 if up >= down else 0.0,
                "down": 1.0 if down > up else 0.0,
                "raw_outcome_prices": outcome_prices,
                "closed": raw_market.get("closed"),
            }
    return {
        "source": "gamma_unresolved",
        "up": math.nan,
        "down": math.nan,
        "raw_outcome_prices": outcome_prices,
        "closed": raw_market.get("closed"),
    }


def infer_settlement_from_spot(
    market: MarketInfo,
    rows: list[dict[str, Any]],
) -> dict[str, Any]:
    market_rows = [r for r in rows if r["market_slug"] == market.slug]
    if not market_rows:
        return {"source": "no_rows", "up": math.nan, "down": math.nan}
    final_row = min(
        market_rows,
        key=lambda row: abs(number(row["recv_ts"]) - market.end_ts),
    )
    spot = number(final_row.get("spot_index_mid"))
    strike = number(final_row.get("round_start_price_k"))
    if not finite(spot) or not finite(strike):
        return {"source": "inferred_spot_missing", "up": math.nan, "down": math.nan}
    up = 1.0 if spot > strike else 0.0
    return {
        "source": "inferred_spot_at_end",
        "up": up,
        "down": 1.0 - up,
        "spot_at_end_sample": spot,
        "K": strike,
        "sample_utc": final_row.get("recv_utc", ""),
    }


def fetch_settlements(
    markets: list[MarketInfo],
    rows: list[dict[str, Any]],
    timeout_sec: int,
    poll_sec: int,
) -> dict[str, dict[str, Any]]:
    deadline = time.time() + timeout_sec
    settlements: dict[str, dict[str, Any]] = {}
    unresolved = {market.slug: market for market in markets}
    while unresolved and time.time() <= deadline:
        for slug, market in list(unresolved.items()):
            try:
                settlement = fetch_settlement_once(market)
            except Exception as exc:
                settlement = {
                    "source": f"gamma_error:{type(exc).__name__}",
                    "up": math.nan,
                    "down": math.nan,
                }
            settlements[slug] = settlement
            if finite(number(settlement.get("up"))) and finite(number(settlement.get("down"))):
                del unresolved[slug]
        if unresolved:
            time.sleep(max(1, poll_sec))
    for slug, market in unresolved.items():
        fallback = infer_settlement_from_spot(market, rows)
        settlements[slug] = fallback | {
            "official_settlement_missing": settlements.get(slug, {}),
        }
    return settlements


def find_at_or_before(
    market_rows: list[dict[str, Any]],
    times: list[float],
    target_ts: float,
) -> dict[str, Any] | None:
    idx = bisect.bisect_right(times, target_ts) - 1
    return market_rows[idx] if idx >= 0 else None


def find_at_or_after(
    market_rows: list[dict[str, Any]],
    times: list[float],
    target_ts: float,
) -> dict[str, Any] | None:
    idx = bisect.bisect_left(times, target_ts)
    return market_rows[idx] if idx < len(market_rows) else None


def tau_bucket(tau: float) -> str:
    if not finite(tau):
        return "unknown"
    for name, lo, hi in TAU_BUCKETS:
        if tau >= lo and tau < hi:
            return name
    if tau >= 300.000001:
        return "tau_240_300"
    return "unknown"


def enrich_rows(
    rows: list[dict[str, Any]],
    qtys: list[float],
    settlements: dict[str, dict[str, Any]],
) -> list[dict[str, Any]]:
    by_market: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_market.setdefault(str(row["market_slug"]), []).append(row)
    for market_rows in by_market.values():
        market_rows.sort(key=lambda row: number(row["recv_ts"]))

    enriched: list[dict[str, Any]] = []
    for market_slug, market_rows in by_market.items():
        times = [number(row["recv_ts"]) for row in market_rows]
        settlement = settlements.get(market_slug, {})
        settlement_up = number(settlement.get("up"))
        settlement_down = number(settlement.get("down"))
        winner = (
            "up" if finite(settlement_up) and settlement_up > settlement_down
            else "down" if finite(settlement_down) and settlement_down > settlement_up
            else "unknown"
        )
        for row in market_rows:
            out = dict(row)
            ts = number(row["recv_ts"])
            for horizon in (1, 3):
                prev = find_at_or_before(market_rows, times, ts - horizon)
                if prev is None:
                    out[f"fair_delta_{horizon}s"] = math.nan
                    out[f"fair_sigma_floor_delta_{horizon}s"] = math.nan
                    out[f"up_mid_delta_{horizon}s"] = math.nan
                    out[f"down_mid_delta_{horizon}s"] = math.nan
                else:
                    out[f"fair_delta_{horizon}s"] = delta(
                        row.get("fair_up"), prev.get("fair_up")
                    )
                    out[f"fair_sigma_floor_delta_{horizon}s"] = delta(
                        row.get("fair_up_sigma_floor"), prev.get("fair_up_sigma_floor")
                    )
                    out[f"up_mid_delta_{horizon}s"] = delta(
                        row.get("up_mid"), prev.get("up_mid")
                    )
                    out[f"down_mid_delta_{horizon}s"] = delta(
                        row.get("down_mid"), prev.get("down_mid")
                    )
            for horizon in MOMENTUM_HORIZONS_SEC:
                prev = find_at_or_before(market_rows, times, ts - horizon)
                out[f"spot_return_{horizon}s"] = (
                    spot_return(row.get("spot_index_mid"), prev.get("spot_index_mid"))
                    if prev is not None
                    else math.nan
                )
            out["tau_bucket"] = tau_bucket(number(row.get("tau_sec")))
            out["settlement_up"] = settlement_up
            out["settlement_down"] = settlement_down
            out["settlement_winner"] = winner
            out["settlement_source"] = settlement.get("source", "")
            for qty in qtys:
                q = qty_label(qty)
                for horizon in FORWARD_HORIZONS_SEC:
                    future = find_at_or_after(market_rows, times, ts + horizon)
                    if future is None:
                        out[f"buy_up_markout_{horizon}s_{q}"] = math.nan
                        out[f"buy_down_markout_{horizon}s_{q}"] = math.nan
                    else:
                        out[f"buy_up_markout_{horizon}s_{q}"] = delta(
                            future.get("up_mid"), row.get(f"vwap_buy_up_{q}")
                        )
                        out[f"buy_down_markout_{horizon}s_{q}"] = delta(
                            future.get("down_mid"), row.get(f"vwap_buy_down_{q}")
                        )
                out[f"buy_up_settlement_pnl_{q}"] = delta(
                    settlement_up, row.get(f"vwap_buy_up_{q}")
                )
                out[f"buy_down_settlement_pnl_{q}"] = delta(
                    settlement_down, row.get(f"vwap_buy_down_{q}")
                )
            enriched.append(out)
    enriched.sort(key=lambda row: number(row["recv_ts"]))
    return enriched


def delta(a: Any, b: Any) -> float:
    av = number(a)
    bv = number(b)
    return av - bv if finite(av) and finite(bv) else math.nan


def spot_return(now_value: Any, prev_value: Any) -> float:
    now_v = number(now_value)
    prev_v = number(prev_value)
    if not finite(now_v) or not finite(prev_v) or prev_v <= 0:
        return math.nan
    return now_v / prev_v - 1.0


def fair_direction_aligned(row: dict[str, Any], side: str, fair_delta_field: str) -> bool:
    fair_delta = number(row.get(fair_delta_field))
    if not finite(fair_delta):
        return False
    if side == "buy_up":
        return fair_delta > 0
    if side == "buy_down":
        return fair_delta < 0
    raise ValueError(side)


def side_fields(
    side: str,
    q: str,
    edge_variant: str = "raw",
) -> dict[str, str]:
    if side == "buy_up":
        return {
            "edge": f"edge_buy_up_{q}" if edge_variant == "raw" else f"edge_buy_up_sigma_floor_{q}",
            "markout_1s": f"buy_up_markout_1s_{q}",
            "markout_3s": f"buy_up_markout_3s_{q}",
            "markout_5s": f"buy_up_markout_5s_{q}",
            "settlement_pnl": f"buy_up_settlement_pnl_{q}",
        }
    if side == "buy_down":
        return {
            "edge": f"edge_buy_down_{q}" if edge_variant == "raw" else f"edge_buy_down_sigma_floor_{q}",
            "markout_1s": f"buy_down_markout_1s_{q}",
            "markout_3s": f"buy_down_markout_3s_{q}",
            "markout_5s": f"buy_down_markout_5s_{q}",
            "settlement_pnl": f"buy_down_settlement_pnl_{q}",
        }
    raise ValueError(side)


def signal_metrics(
    rows: list[dict[str, Any]],
    side: str,
    q: str,
    edge_variant: str = "raw",
) -> dict[str, Any]:
    fields = side_fields(side, q, edge_variant=edge_variant)
    values = {name: [number(row.get(field)) for row in rows] for name, field in fields.items()}
    markets = {str(row.get("market_slug")) for row in rows}
    up_winning_markets = {
        str(row.get("market_slug"))
        for row in rows
        if row.get("settlement_winner") == "up"
    }
    down_winning_markets = {
        str(row.get("market_slug"))
        for row in rows
        if row.get("settlement_winner") == "down"
    }
    return {
        "count": len(rows),
        "market_count": len(markets),
        "avg_edge": mean(values["edge"]),
        "markout_1s": mean(values["markout_1s"]),
        "markout_3s": mean(values["markout_3s"]),
        "markout_5s": mean(values["markout_5s"]),
        "settlement_pnl": mean(values["settlement_pnl"]),
        "positive_markout_1s": sum(1 for v in values["markout_1s"] if finite(v) and v > 0),
        "positive_markout_3s": sum(1 for v in values["markout_3s"] if finite(v) and v > 0),
        "positive_markout_5s": sum(1 for v in values["markout_5s"] if finite(v) and v > 0),
        "positive_settlement_pnl": sum(1 for v in values["settlement_pnl"] if finite(v) and v > 0),
        "avg_tau_sec": mean([number(row.get("tau_sec")) for row in rows]),
        "signals_in_up_winning_markets": sum(1 for row in rows if row.get("settlement_winner") == "up"),
        "signals_in_down_winning_markets": sum(1 for row in rows if row.get("settlement_winner") == "down"),
        "unique_up_winning_markets": len(up_winning_markets),
        "unique_down_winning_markets": len(down_winning_markets),
        "sample_markets": sorted(markets)[:10],
    }


def edge_rows(
    rows: list[dict[str, Any]],
    side: str,
    q: str,
    edge_variant: str = "raw",
    require_fair_direction: bool = False,
    fair_delta_field: str = "fair_delta_1s",
) -> list[dict[str, Any]]:
    edge_field = side_fields(side, q, edge_variant=edge_variant)["edge"]
    out = []
    for row in rows:
        if number(row.get(edge_field)) <= EDGE_THRESHOLD:
            continue
        if require_fair_direction and not fair_direction_aligned(row, side, fair_delta_field):
            continue
        out.append(row)
    return out


def build_four_tables(
    rows: list[dict[str, Any]],
    qtys: list[float],
    base_qty: float,
    sigma_floor: float,
) -> dict[str, Any]:
    q_base = qty_label(base_qty)
    sides = ("buy_up", "buy_down")

    table1: dict[str, Any] = {}
    for side in sides:
        rows_edge = edge_rows(rows, side, q_base)
        aligned = [row for row in rows_edge if fair_direction_aligned(row, side, "fair_delta_1s")]
        opposed = [row for row in rows_edge if not fair_direction_aligned(row, side, "fair_delta_1s")]
        table1[side] = {
            "fair_delta_1s_aligned": signal_metrics(aligned, side, q_base),
            "fair_delta_1s_opposed_or_missing": signal_metrics(opposed, side, q_base),
        }

    table2: dict[str, Any] = {}
    for side in sides:
        side_rows = edge_rows(rows, side, q_base, require_fair_direction=True)
        table2[side] = {}
        for bucket_name, _, _ in TAU_BUCKETS:
            bucket_rows = [row for row in side_rows if row.get("tau_bucket") == bucket_name]
            table2[side][bucket_name] = signal_metrics(bucket_rows, side, q_base)

    table3: dict[str, Any] = {"sigma_floor_annualized": sigma_floor}
    for side in sides:
        sigma_rows = edge_rows(
            rows,
            side,
            q_base,
            edge_variant="sigma_floor",
            require_fair_direction=True,
            fair_delta_field="fair_sigma_floor_delta_1s",
        )
        table3[side] = signal_metrics(sigma_rows, side, q_base, edge_variant="sigma_floor")

    table4: dict[str, Any] = {}
    for qty in (25.0, 50.0):
        if qty not in qtys:
            continue
        q = qty_label(qty)
        table4[f"qty_{qty:g}"] = {}
        for side in sides:
            qty_rows = edge_rows(rows, side, q, require_fair_direction=True)
            table4[f"qty_{qty:g}"][side] = signal_metrics(qty_rows, side, q)

    buy_up_candidate = table1["buy_up"]["fair_delta_1s_aligned"]
    candidate_alpha_check = {
        "rule": f"buy_up && edge>{EDGE_THRESHOLD:.2f} && fair_delta_1s>0",
        "markout_3s_positive": number(buy_up_candidate.get("markout_3s")) > 0,
        "markout_5s_positive": number(buy_up_candidate.get("markout_5s")) > 0,
        "settlement_pnl_positive": number(buy_up_candidate.get("settlement_pnl")) > 0,
        "not_only_up_winning_markets": buy_up_candidate.get("unique_down_winning_markets", 0) > 0,
        "metrics": buy_up_candidate,
    }
    return {
        "definitions": {
            "edge_threshold": EDGE_THRESHOLD,
            "base_qty": base_qty,
            "fair_direction": {
                "buy_up": "fair_delta_1s > 0",
                "buy_down": "fair_delta_1s < 0",
            },
            "markout": "future same-token Polymarket mid at t+h minus taker VWAP entry",
            "settlement_pnl": "resolved payout minus taker VWAP entry",
            "tau_buckets": [name for name, _, _ in TAU_BUCKETS],
        },
        "edge_gt_3c_by_side_and_fair_direction": table1,
        "same_rule_by_tau_bucket": table2,
        "same_rule_after_sigma_floor": table3,
        "same_rule_after_qty_25_50": table4,
        "candidate_alpha_check": candidate_alpha_check,
    }


def source_coverage(rows: list[dict[str, Any]]) -> dict[str, Any]:
    total = len(rows)
    keys = [
        "source_ok_coinbase",
        "source_ok_binance_spot",
        "source_ok_binance_perp",
        "source_ok_deribit",
        "source_ok_poly_up",
        "source_ok_poly_down",
    ]
    return {
        key.removeprefix("source_ok_"): {
            "ok": sum(1 for row in rows if int(number(row.get(key), 0)) == 1),
            "total": total,
        }
        for key in keys
    }


def market_capture_summary(rows: list[dict[str, Any]], markets: list[MarketInfo]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for market in markets:
        market_rows = [row for row in rows if row["market_slug"] == market.slug]
        if not market_rows:
            out[market.slug] = {"rows": 0}
            continue
        first = market_rows[0]
        last = market_rows[-1]
        taus = [number(row.get("tau_sec")) for row in market_rows]
        out[market.slug] = {
            "rows": len(market_rows),
            "first_utc": first["recv_utc"],
            "last_utc": last["recv_utc"],
            "first_tau_sec": max(taus) if taus else math.nan,
            "last_tau_sec": min(taus) if taus else math.nan,
            "K": number(first.get("round_start_price_k")),
            "k_source": first.get("k_source", ""),
            "source_coverage": source_coverage(market_rows),
        }
    return out


def capture_sample(
    sample_index: int,
    state: RoundState,
    qtys: list[float],
    spot_window: list[tuple[float, float]],
    vol_window_sec: int,
    default_sigma: float,
    sigma_floor: float,
    errors: dict[str, int],
) -> tuple[dict[str, Any], dict[str, Any], list[tuple[float, float]]]:
    recv_ts = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        futures = {
            "up": pool.submit(fetch_safe, errors, "poly_up", lambda: fetch_poly_book(state.market.up_token)),
            "down": pool.submit(fetch_safe, errors, "poly_down", lambda: fetch_poly_book(state.market.down_token)),
            "coinbase": pool.submit(fetch_safe, errors, "coinbase", fetch_coinbase),
            "binance_spot": pool.submit(fetch_binance_spot),
            "binance_perp": pool.submit(fetch_binance_perp),
            "deribit": pool.submit(fetch_safe, errors, "deribit", fetch_deribit_perp),
        }
        up_book = normalize_book(futures["up"].result(), errors, "poly_up_bad_book")
        down_book = normalize_book(futures["down"].result(), errors, "poly_down_bad_book")
        coinbase = futures["coinbase"].result()
        binance_spot = futures["binance_spot"].result()
        binance_perp = futures["binance_perp"].result()
        deribit = futures["deribit"].result()

    spot_index_mid = spot_index_mid_from_sources(
        coinbase, binance_spot, binance_perp, deribit
    )
    if finite(spot_index_mid):
        spot_window.append((recv_ts, spot_index_mid))
        cutoff = recv_ts - vol_window_sec
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
    sigma = realized_vol_annualized(spot_window, default_sigma)
    sigma_floor_used = max(sigma, sigma_floor)
    fair_up = fair_up_probability(spot_index_mid, state.strike, sigma, tau_sec)
    fair_up_floor = fair_up_probability(
        spot_index_mid,
        state.strike,
        sigma_floor_used,
        tau_sec,
    )
    row = row_from_live_sample(
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
        sigma_floor_used,
        fair_up,
        fair_up_floor,
        qtys,
    )
    clob = {
        "sample_index": sample_index,
        "recv_ts": recv_ts,
        "recv_utc": now_iso(recv_ts),
        "market_slug": state.market.slug,
        "up_token": state.market.up_token,
        "down_token": state.market.down_token,
        "up_bids": up_book["bids"],
        "up_asks": up_book["asks"],
        "down_bids": down_book["bids"],
        "down_asks": down_book["asks"],
    }
    return row, clob, spot_window


def write_report(path: Path, tables: dict[str, Any], metadata: dict[str, Any]) -> None:
    candidate = tables["candidate_alpha_check"]
    lines = [
        "# BTC 5m Full-Market Taker Diagnostics",
        "",
        f"- generated_at_utc: {metadata['generated_at_utc']}",
        f"- market_count_requested: {metadata['market_count_requested']}",
        f"- interval_ms: {metadata['interval_ms']}",
        f"- qtys: {', '.join(str(q) for q in metadata['qtys'])}",
        f"- sigma_floor_annualized: {metadata['sigma_floor_annualized']}",
        "",
        "## Candidate Check",
        f"- rule: {candidate['rule']}",
        f"- markout_3s_positive: {candidate['markout_3s_positive']}",
        f"- markout_5s_positive: {candidate['markout_5s_positive']}",
        f"- settlement_pnl_positive: {candidate['settlement_pnl_positive']}",
        f"- not_only_up_winning_markets: {candidate['not_only_up_winning_markets']}",
        "",
        "See four_tables.json for the four requested tables.",
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--market-count", type=int, default=20)
    parser.add_argument("--interval-ms", type=int, default=1000)
    parser.add_argument("--qtys", default="5,25,50")
    parser.add_argument("--base-qty", type=float, default=5.0)
    parser.add_argument("--vol-window-sec", type=int, default=60)
    parser.add_argument("--default-sigma", type=float, default=0.65)
    parser.add_argument("--sigma-floor", type=float, default=0.65)
    parser.add_argument("--settlement-timeout-sec", type=int, default=900)
    parser.add_argument("--settlement-poll-sec", type=int, default=15)
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--start-ts", type=int, default=0)
    args = parser.parse_args()

    if args.market_count <= 0:
        raise ValueError("--market-count must be positive")
    if args.interval_ms <= 0:
        raise ValueError("--interval-ms must be positive")
    qtys = [float(part) for part in args.qtys.split(",") if part.strip()]
    if args.base_qty not in qtys:
        qtys.insert(0, args.base_qty)
    qtys = sorted(set(qtys))
    fields = csv_fields(qtys)
    enriched_fields = csv_fields(qtys, enriched=True)

    start_ts = args.start_ts or next_round_start()
    out_dir = Path(args.out_dir or f"research/runs/btc5m_full_markets_{utc_stamp()}")
    out_dir.mkdir(parents=True, exist_ok=True)
    samples_csv = out_dir / "samples.csv"
    clob_jsonl = out_dir / "clob_snapshots.jsonl"
    enriched_csv = out_dir / "samples_enriched.csv"
    settlements_path = out_dir / "settlements.json"
    tables_path = out_dir / "four_tables.json"
    metadata_path = out_dir / "metadata.json"
    report_path = out_dir / "report.md"

    errors: dict[str, int] = {}
    rows: list[dict[str, Any]] = []
    markets: list[MarketInfo] = []
    spot_window: list[tuple[float, float]] = []
    sample_index = 0
    started = time.time()

    metadata = {
        "generated_at_utc": now_iso(),
        "market_count_requested": args.market_count,
        "interval_ms": args.interval_ms,
        "qtys": qtys,
        "base_qty": args.base_qty,
        "vol_window_sec": args.vol_window_sec,
        "default_sigma": args.default_sigma,
        "sigma_floor_annualized": args.sigma_floor,
        "start_ts": start_ts,
        "start_utc": now_iso(start_ts),
        "csv": str(samples_csv),
        "clob_jsonl": str(clob_jsonl),
        "enriched_csv": str(enriched_csv),
        "settlements": str(settlements_path),
        "four_tables": str(tables_path),
        "metadata": str(metadata_path),
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
    }
    metadata_path.write_text(json.dumps(sanitize_json(metadata), indent=2), encoding="utf-8")

    print(f"out_dir={out_dir}", flush=True)
    print(f"start_utc={now_iso(start_ts)} market_count={args.market_count}", flush=True)
    print(f"interval_ms={args.interval_ms} qtys={qtys}", flush=True)

    with samples_csv.open("w", newline="", encoding="utf-8") as csv_fh, clob_jsonl.open(
        "w", encoding="utf-8"
    ) as clob_fh:
        writer = csv.DictWriter(csv_fh, fieldnames=fields)
        writer.writeheader()
        for market_idx in range(args.market_count):
            market_start = start_ts + market_idx * 300
            sleep_until(market_start)
            market = fetch_market(market_start)
            markets.append(market)
            state = RoundState(market=market)
            print(
                f"market_start idx={market_idx + 1}/{args.market_count} "
                f"slug={market.slug} utc={now_iso(market_start)}",
                flush=True,
            )
            next_sample_target = market_start
            while time.time() <= market.end_ts + 0.05:
                sleep_until(next_sample_target)
                row, clob, spot_window = capture_sample(
                    sample_index,
                    state,
                    qtys,
                    spot_window,
                    args.vol_window_sec,
                    args.default_sigma,
                    args.sigma_floor,
                    errors,
                )
                write_csv_row(writer, fields, row)
                csv_fh.flush()
                write_jsonl_row(clob_fh, clob)
                rows.append(row)
                sample_index += 1
                if sample_index % max(1, round(30_000 / args.interval_ms)) == 0:
                    print(
                        f"progress samples={sample_index} market={market.slug} "
                        f"tau={fmt(row['tau_sec'])} "
                        f"edge_up={fmt(row.get('edge_buy_up_' + qty_label(args.base_qty)))} "
                        f"edge_down={fmt(row.get('edge_buy_down_' + qty_label(args.base_qty)))}",
                        flush=True,
                    )
                next_sample_target += args.interval_ms / 1000.0
                if next_sample_target <= time.time() - 2.0:
                    next_sample_target = time.time()
            print(
                f"market_done slug={market.slug} rows={sum(1 for r in rows if r['market_slug'] == market.slug)}",
                flush=True,
            )

    print("fetching_settlements=true", flush=True)
    settlements = fetch_settlements(
        markets,
        rows,
        args.settlement_timeout_sec,
        args.settlement_poll_sec,
    )
    settlements_path.write_text(
        json.dumps(sanitize_json(settlements), indent=2),
        encoding="utf-8",
    )
    enriched = enrich_rows(rows, qtys, settlements)
    with enriched_csv.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=enriched_fields)
        writer.writeheader()
        for row in enriched:
            write_csv_row(writer, enriched_fields, row)

    tables = build_four_tables(enriched, qtys, args.base_qty, args.sigma_floor)
    full_tables = {
        "metadata": metadata
        | {
            "finished_at_utc": now_iso(),
            "elapsed_sec": time.time() - started,
            "rows": len(enriched),
            "errors": errors,
            "markets": [dataclasses.asdict(market) for market in markets],
            "market_capture_summary": market_capture_summary(enriched, markets),
            "source_coverage": source_coverage(enriched),
        },
        "tables": tables,
    }
    tables_path.write_text(
        json.dumps(sanitize_json(full_tables), indent=2),
        encoding="utf-8",
    )
    write_report(report_path, tables, full_tables["metadata"])
    metadata_path.write_text(
        json.dumps(sanitize_json(full_tables["metadata"]), indent=2),
        encoding="utf-8",
    )
    print(f"samples_csv={samples_csv}", flush=True)
    print(f"clob_jsonl={clob_jsonl}", flush=True)
    print(f"enriched_csv={enriched_csv}", flush=True)
    print(f"settlements={settlements_path}", flush=True)
    print(f"four_tables={tables_path}", flush=True)
    print(f"rows={len(enriched)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

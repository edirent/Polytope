#!/usr/bin/env python3
"""Read-only Polymarket BTC Above fair-value research.

This script is intentionally research-only: it connects only to public market-data
endpoints, never signs orders, never calls trading endpoints, and never computes
execution instructions. It compares BTC-derived digital-option fair value against
Polymarket order-book implied probabilities for a fixed research target by default:
Bitcoin above 62,000 on June 6, 2026, resolving against the Binance BTCUSDT
1-minute candle close at 2026-06-06T16:00:00+00:00.

Strict definitions used here:
- raw_fair_yes: P(BTC_T > threshold) from a lognormal digital-call model.
- poly_yes_mid_direct: midpoint of the YES token book.
- poly_yes_mid_from_no: 1 - midpoint of the NO token book.
- poly_yes_mid_composite: mean of direct and NO-implied mids when both are healthy.
- deviation_direct: poly_yes_mid_direct - raw_fair_yes.
- deviation_composite: poly_yes_mid_composite - raw_fair_yes.
- basis_ewma: time-decayed EWMA of deviation_composite on healthy samples only.
- residual: deviation_composite - basis_ewma.

Every stage_interval_seconds, the script writes a stage JSON file and appends one
line to stages/stage_summaries.jsonl. Final CSV, summary, and plots are written
at the end.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import datetime as dt
import json
import math
from pathlib import Path
import re
import statistics
import threading
import time
from typing import Any
import urllib.parse
import urllib.request

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import websocket  # noqa: E402


BINANCE_REST_PRICE = "https://api.binance.com/api/v3/ticker/price"
BINANCE_VISION_WS = "wss://data-stream.binance.vision/ws"
POLYMARKET_WS = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
GAMMA_MARKETS = "https://gamma-api.polymarket.com/markets"
DERIBIT_API = "https://www.deribit.com/api/v2"
SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0
PROB_EPS = 1e-9


def now_ns() -> int:
    return time.time_ns()


def utc_stamp() -> str:
    return time.strftime("%Y%m%d_%H%M%S", time.gmtime())


def iso_utc_from_unix(seconds: float) -> str:
    return dt.datetime.fromtimestamp(seconds, tz=dt.timezone.utc).isoformat()


def http_json(url: str, timeout_s: int = 15) -> Any:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "PolytopeBTCAboveFairResearch/0.1",
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
            return parsed if isinstance(parsed, list) else []
        except json.JSONDecodeError:
            return []
    return []


def number(value: Any, default: float | None = math.nan) -> float:
    if value is None or value == "":
        return default if default is not None else math.nan
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default if default is not None else math.nan
    return parsed if math.isfinite(parsed) else (default if default is not None else math.nan)


def norm_cdf(value: float) -> float:
    return 0.5 * math.erfc(-value / math.sqrt(2.0))


def clamp_prob(value: float) -> float:
    if not math.isfinite(value):
        return math.nan
    return min(1.0 - PROB_EPS, max(PROB_EPS, value))


def logit(value: float) -> float:
    p = clamp_prob(value)
    return math.log(p / (1.0 - p)) if math.isfinite(p) else math.nan


def digital_call_probability(
    *,
    spot: float,
    strike: float,
    seconds_to_expiry: float,
    vol_annual_bps: float,
    drift_annual_bps: float,
) -> float:
    """Risk-neutral/lognormal P(S_T > K) under fixed drift and volatility."""
    if not all(math.isfinite(v) for v in (spot, strike, seconds_to_expiry, vol_annual_bps, drift_annual_bps)):
        return math.nan
    if spot <= 0 or strike <= 0 or vol_annual_bps <= 0:
        return math.nan
    if seconds_to_expiry <= 0:
        return 1.0 if spot > strike else 0.0
    years = seconds_to_expiry / SECONDS_PER_YEAR
    sigma = vol_annual_bps / 10_000.0
    drift = drift_annual_bps / 10_000.0
    denom = sigma * math.sqrt(years)
    if denom <= 0 or not math.isfinite(denom):
        return math.nan
    d2 = (math.log(spot / strike) + (drift - 0.5 * sigma * sigma) * years) / denom
    return max(0.0, min(1.0, norm_cdf(d2)))


def norm_pdf(value: float) -> float:
    return math.exp(-0.5 * value * value) / math.sqrt(2.0 * math.pi)


def bs_call_undiscounted(forward: float, strike: float, years: float, sigma: float) -> float:
    """Black-76 call price under a forward measure with zero rate (undiscounted)."""
    if not all(math.isfinite(v) for v in (forward, strike, years, sigma)):
        return math.nan
    if years <= 0:
        return max(0.0, forward - strike)
    if forward <= 0 or strike <= 0 or sigma <= 0:
        return math.nan
    sd = sigma * math.sqrt(years)
    if sd <= 0:
        return max(0.0, forward - strike)
    d1 = (math.log(forward / strike) + 0.5 * sigma * sigma * years) / sd
    d2 = d1 - sd
    return forward * norm_cdf(d1) - strike * norm_cdf(d2)


def bs_digital_call_nd2(forward: float, strike: float, years: float, sigma: float) -> float:
    """Smile-free digital P(F_T > K) = N(d2). Skew is ignored on purpose here."""
    if not all(math.isfinite(v) for v in (forward, strike, years, sigma)):
        return math.nan
    if years <= 0:
        return 1.0 if forward > strike else 0.0
    if forward <= 0 or strike <= 0 or sigma <= 0:
        return math.nan
    sd = sigma * math.sqrt(years)
    if sd <= 0:
        return 1.0 if forward > strike else 0.0
    d2 = (math.log(forward / strike) - 0.5 * sigma * sigma * years) / sd
    return max(0.0, min(1.0, norm_cdf(d2)))


def interp_smile(points: list[tuple[float, float]], strike: float) -> float:
    """Linear IV interpolation across strikes with flat extrapolation at the wings."""
    pts = sorted((k, v) for k, v in points if math.isfinite(k) and math.isfinite(v) and v > 0)
    if not pts:
        return math.nan
    if strike <= pts[0][0]:
        return pts[0][1]
    if strike >= pts[-1][0]:
        return pts[-1][1]
    for (k0, v0), (k1, v1) in zip(pts[:-1], pts[1:]):
        if k0 <= strike <= k1:
            if k1 == k0:
                return v0
            weight = (strike - k0) / (k1 - k0)
            return v0 + weight * (v1 - v0)
    return pts[-1][1]


def percentile(values: list[float], q: float) -> float:
    clean = sorted(v for v in values if math.isfinite(v))
    if not clean:
        return math.nan
    idx = min(len(clean) - 1, max(0, int(round((len(clean) - 1) * q))))
    return clean[idx]


def safe_mean(values: list[float]) -> float:
    clean = [v for v in values if math.isfinite(v)]
    return statistics.mean(clean) if clean else math.nan


def safe_median(values: list[float]) -> float:
    clean = [v for v in values if math.isfinite(v)]
    return statistics.median(clean) if clean else math.nan


def parse_datetime_to_unix(value: Any) -> int:
    if not value:
        return 0
    text = str(value)
    try:
        if text.endswith("Z"):
            text = text[:-1] + "+00:00"
        return int(dt.datetime.fromisoformat(text).timestamp())
    except ValueError:
        return 0


def parse_threshold_from_question(question: str) -> float:
    """Parse threshold from questions like 'Bitcoin above $64,000 on June 5?'"""
    patterns = [
        r"bitcoin\s+above\s+\$?([0-9][0-9,]*(?:\.\d+)?)",
        r"btc\s+above\s+\$?([0-9][0-9,]*(?:\.\d+)?)",
        r"above\s+\$?([0-9][0-9,]*(?:\.\d+)?)",
    ]
    lower = question.lower()
    for pattern in patterns:
        match = re.search(pattern, lower)
        if match:
            return float(match.group(1).replace(",", ""))
    return math.nan


@dataclasses.dataclass(frozen=True)
class MarketInfo:
    slug: str
    question: str
    condition_id: str
    market_id: str
    end_ts: int
    threshold: float
    yes_token_id: str
    no_token_id: str


def market_from_gamma_row(row: dict[str, Any], fallback_window_end_ts: int = 0) -> MarketInfo | None:
    question = str(row.get("question") or row.get("title") or row.get("slug") or "")
    threshold = parse_threshold_from_question(question)
    outcomes = [str(x) for x in parse_json_array(row.get("outcomes"))]
    tokens = [str(x) for x in parse_json_array(row.get("clobTokenIds"))]
    if len(tokens) < 2 or len(outcomes) < 2 or not math.isfinite(threshold):
        return None

    lowered = [x.strip().lower() for x in outcomes]
    yes_index = next((i for i, x in enumerate(lowered) if x in {"yes", "up"}), 0)
    no_index = next((i for i, x in enumerate(lowered) if x in {"no", "down"}), 1 if len(tokens) > 1 else 0)
    if yes_index >= len(tokens) or no_index >= len(tokens) or yes_index == no_index:
        return None

    end_ts = (
        parse_datetime_to_unix(row.get("endDate"))
        or parse_datetime_to_unix(row.get("endDateIso"))
        or parse_datetime_to_unix(row.get("closedTime"))
        or int(fallback_window_end_ts)
    )

    return MarketInfo(
        slug=str(row.get("slug") or row.get("id") or question),
        question=question,
        condition_id=str(row.get("conditionId") or ""),
        market_id=str(row.get("conditionId") or row.get("id") or row.get("slug") or question),
        end_ts=end_ts,
        threshold=threshold,
        yes_token_id=tokens[yes_index],
        no_token_id=tokens[no_index],
    )


def fetch_market_by_slug(slug: str, fallback_window_end_ts: int = 0) -> MarketInfo:
    query = urllib.parse.urlencode({"slug": slug})
    data = http_json(f"{GAMMA_MARKETS}?{query}")
    if not isinstance(data, list) or not data:
        raise RuntimeError(f"Polymarket market not found for slug={slug}")
    market = market_from_gamma_row(data[0], fallback_window_end_ts)
    if market is None:
        raise RuntimeError(f"Could not parse market slug={slug}: {data[0]}")
    return market


def discover_btc_above_markets(args: argparse.Namespace) -> list[MarketInfo]:
    if args.market_slugs:
        return [fetch_market_by_slug(slug.strip(), args.window_end_unix_seconds) for slug in args.market_slugs.split(",") if slug.strip()]

    candidates: dict[str, MarketInfo] = {}
    offsets = list(range(0, max(args.gamma_scan_limit, 500), 500))
    for offset in offsets:
        query_params = {
            "limit": 500,
            "offset": offset,
            "active": "true",
            "closed": "false" if not args.include_closed else "true",
        }
        url = f"{GAMMA_MARKETS}?{urllib.parse.urlencode(query_params)}"
        data = http_json(url)
        if not isinstance(data, list) or not data:
            break
        for row in data:
            if not isinstance(row, dict):
                continue
            question = str(row.get("question") or row.get("title") or "")
            q_lower = question.lower()
            if "bitcoin" not in q_lower and "btc" not in q_lower:
                continue
            if "above" not in q_lower:
                continue
            if args.date_label and args.date_label.lower() not in q_lower:
                continue
            market = market_from_gamma_row(row, args.window_end_unix_seconds)
            if market is None:
                continue
            candidates[market.market_id] = market
        if len(data) < 500:
            break

    markets = list(candidates.values())
    if args.thresholds:
        wanted = {float(x.strip().replace(",", "")) for x in args.thresholds.split(",") if x.strip()}
        markets = [m for m in markets if m.threshold in wanted]
    if not markets:
        raise RuntimeError(
            "No matching BTC Above markets found. Provide --market-slugs or relax --date-label."
        )

    reference_spot = args.reference_spot
    if not math.isfinite(reference_spot) or reference_spot <= 0:
        reference_spot = fetch_binance_spot_once(default=math.nan)
    if math.isfinite(reference_spot) and reference_spot > 0:
        markets.sort(key=lambda m: (abs(m.threshold - reference_spot), m.threshold))
    else:
        markets.sort(key=lambda m: m.threshold)
    return markets[: args.max_markets]


def fetch_binance_spot_once(default: float = math.nan) -> float:
    try:
        query = urllib.parse.urlencode({"symbol": "BTCUSDT"})
        data = http_json(f"{BINANCE_REST_PRICE}?{query}", timeout_s=5)
        return number(data.get("price"), default) if isinstance(data, dict) else default
    except Exception:
        return default


class OracleState:
    def __init__(self, *, vol_window_seconds: float) -> None:
        self.lock = threading.RLock()
        self.bid = math.nan
        self.ask = math.nan
        self.bid_qty = math.nan
        self.ask_qty = math.nan
        self.mid = math.nan
        self.depth5_bid_qty = math.nan
        self.depth5_ask_qty = math.nan
        self.depth5_imbalance = math.nan
        self.last_bbo: tuple[float, float, float, float] | None = None
        self.mid_events: list[tuple[int, float]] = []
        self.ofi_events: list[tuple[int, float]] = []
        self.book_ticker_events = 0
        self.depth_events = 0
        self.errors: list[str] = []
        self.vol_window_ns = int(vol_window_seconds * 1_000_000_000)

    def observe_book_ticker(self, payload: dict[str, Any]) -> None:
        bid = number(payload.get("b"))
        ask = number(payload.get("a"))
        bid_qty = number(payload.get("B"))
        ask_qty = number(payload.get("A"))
        if not all(math.isfinite(x) and x > 0 for x in (bid, ask, bid_qty, ask_qty)):
            return
        mid = (bid + ask) / 2.0
        ts = now_ns()
        with self.lock:
            ofi = 0.0
            if self.last_bbo is not None:
                prev_bid, prev_ask, prev_bid_qty, prev_ask_qty = self.last_bbo
                if bid >= prev_bid:
                    ofi += bid_qty
                if bid <= prev_bid:
                    ofi -= prev_bid_qty
                if ask <= prev_ask:
                    ofi -= ask_qty
                if ask >= prev_ask:
                    ofi += prev_ask_qty
                self.ofi_events.append((ts, ofi))
            self.last_bbo = (bid, ask, bid_qty, ask_qty)
            self.bid = bid
            self.ask = ask
            self.bid_qty = bid_qty
            self.ask_qty = ask_qty
            self.mid = mid
            self.mid_events.append((ts, mid))
            cutoff = ts - max(self.vol_window_ns, 10_000_000_000)
            while self.mid_events and self.mid_events[0][0] < cutoff:
                self.mid_events.pop(0)
            ofi_cutoff = ts - 10_000_000_000
            while self.ofi_events and self.ofi_events[0][0] < ofi_cutoff:
                self.ofi_events.pop(0)
            self.book_ticker_events += 1

    def observe_depth5(self, payload: dict[str, Any]) -> None:
        bids = payload.get("b") if isinstance(payload.get("b"), list) else []
        asks = payload.get("a") if isinstance(payload.get("a"), list) else []
        bid_qty = sum(number(level[1], 0.0) for level in bids[:5] if isinstance(level, list) and len(level) >= 2)
        ask_qty = sum(number(level[1], 0.0) for level in asks[:5] if isinstance(level, list) and len(level) >= 2)
        denom = bid_qty + ask_qty
        imbalance = (bid_qty - ask_qty) / denom if denom > 0 else math.nan
        with self.lock:
            self.depth5_bid_qty = bid_qty
            self.depth5_ask_qty = ask_qty
            self.depth5_imbalance = imbalance
            self.depth_events += 1

    def realized_vol_annual_bps(self) -> float:
        with self.lock:
            events = list(self.mid_events)
        if len(events) < 3:
            return math.nan
        returns: list[float] = []
        for (_, prev), (_, cur) in zip(events[:-1], events[1:]):
            if prev > 0 and cur > 0:
                returns.append(math.log(cur / prev))
        if not returns:
            return math.nan
        elapsed_sec = max(1e-6, (events[-1][0] - events[0][0]) / 1_000_000_000.0)
        variance_per_sec = sum(r * r for r in returns) / elapsed_sec
        return math.sqrt(max(0.0, variance_per_sec) * SECONDS_PER_YEAR) * 10_000.0

    def snapshot(self, ts_ns: int) -> dict[str, float]:
        with self.lock:
            ofi_100 = sum(v for t, v in self.ofi_events if t >= ts_ns - 100_000_000)
            ofi_500 = sum(v for t, v in self.ofi_events if t >= ts_ns - 500_000_000)
            return {
                "spot_bid": self.bid,
                "spot_ask": self.ask,
                "spot_bid_qty": self.bid_qty,
                "spot_ask_qty": self.ask_qty,
                "spot_mid": self.mid,
                "spot_depth5_bid_qty": self.depth5_bid_qty,
                "spot_depth5_ask_qty": self.depth5_ask_qty,
                "spot_depth5_imbalance": self.depth5_imbalance,
                "ofi_100ms": ofi_100,
                "ofi_500ms": ofi_500,
                "realized_vol_annual_bps": self.realized_vol_annual_bps(),
            }


@dataclasses.dataclass
class PolyBook:
    bids: dict[float, float] = dataclasses.field(default_factory=dict)
    asks: dict[float, float] = dataclasses.field(default_factory=dict)
    best_bid: float = math.nan
    best_ask: float = math.nan
    last_trade_price: float = math.nan
    last_update_ns: int = 0

    def update_best(self) -> None:
        self.best_bid = max((p for p, s in self.bids.items() if s > 0), default=math.nan)
        self.best_ask = min((p for p, s in self.asks.items() if s > 0), default=math.nan)

    def depth5(self) -> tuple[float, float]:
        top_bids = sorted(((p, s) for p, s in self.bids.items() if s > 0), reverse=True)[:5]
        top_asks = sorted(((p, s) for p, s in self.asks.items() if s > 0))[:5]
        return sum(s for _, s in top_bids), sum(s for _, s in top_asks)


class PolyState:
    def __init__(self, markets: list[MarketInfo]) -> None:
        self.markets = markets
        self.lock = threading.Lock()
        self.books: dict[str, PolyBook] = {}
        self.asset_to_market_side: dict[str, tuple[str, str]] = {}
        for market in markets:
            self.books[market.yes_token_id] = PolyBook()
            self.books[market.no_token_id] = PolyBook()
            self.asset_to_market_side[market.yes_token_id] = (market.market_id, "yes")
            self.asset_to_market_side[market.no_token_id] = (market.market_id, "no")
        self.book_snapshots = 0
        self.price_changes = 0
        self.ws_messages = 0
        self.errors: list[str] = []

    def observe_ws_payload(self, payload: Any) -> None:
        if isinstance(payload, list):
            for item in payload:
                self.observe_ws_payload(item)
            return
        if not isinstance(payload, dict):
            return
        event_type = str(payload.get("event_type") or payload.get("type") or "")
        with self.lock:
            self.ws_messages += 1
        if event_type == "book" or ("bids" in payload and "asks" in payload and "asset_id" in payload):
            self._observe_book(payload)
        elif event_type == "price_change" or "price_changes" in payload:
            self._observe_price_change(payload)

    def _observe_book(self, payload: dict[str, Any]) -> None:
        asset = str(payload.get("asset_id") or "")
        if asset not in self.books:
            return
        book = PolyBook()
        for level in payload.get("bids", []) or []:
            if isinstance(level, dict):
                price = number(level.get("price"))
                size = number(level.get("size"), 0.0)
            else:
                price = size = math.nan
            if math.isfinite(price) and price > 0 and size > 0:
                book.bids[price] = size
        for level in payload.get("asks", []) or []:
            if isinstance(level, dict):
                price = number(level.get("price"))
                size = number(level.get("size"), 0.0)
            else:
                price = size = math.nan
            if math.isfinite(price) and price > 0 and size > 0:
                book.asks[price] = size
        book.last_trade_price = number(payload.get("last_trade_price"))
        book.update_best()
        book.last_update_ns = now_ns()
        with self.lock:
            self.books[asset] = book
            self.book_snapshots += 1

    def _observe_price_change(self, payload: dict[str, Any]) -> None:
        changes = payload.get("price_changes")
        if not isinstance(changes, list):
            return
        ts = now_ns()
        with self.lock:
            for change in changes:
                if not isinstance(change, dict):
                    continue
                asset = str(change.get("asset_id") or "")
                if asset not in self.books:
                    continue
                price = number(change.get("price"))
                size = number(change.get("size"), 0.0)
                side = str(change.get("side") or "").upper()
                book = self.books[asset]
                if math.isfinite(price) and price > 0:
                    levels = book.bids if side == "BUY" else book.asks
                    if size <= 0:
                        levels.pop(price, None)
                    else:
                        levels[price] = size
                best_bid = number(change.get("best_bid"))
                best_ask = number(change.get("best_ask"))
                if math.isfinite(best_bid) and best_bid > 0:
                    book.best_bid = best_bid
                if math.isfinite(best_ask) and best_ask > 0:
                    book.best_ask = best_ask
                if not math.isfinite(book.best_bid) or not math.isfinite(book.best_ask):
                    book.update_best()
                book.last_update_ns = ts
            self.price_changes += len(changes)

    def snapshot_market(self, market: MarketInfo, ts_ns: int, *, max_healthy_spread: float, max_quote_age_ms: float) -> dict[str, float | str | bool]:
        with self.lock:
            yes = dataclasses.replace(self.books[market.yes_token_id])
            no = dataclasses.replace(self.books[market.no_token_id])
        yes_bid, yes_ask = yes.best_bid, yes.best_ask
        no_bid, no_ask = no.best_bid, no.best_ask
        yes_mid = (yes_bid + yes_ask) / 2.0 if all(math.isfinite(x) and x > 0 for x in (yes_bid, yes_ask)) else math.nan
        no_mid = (no_bid + no_ask) / 2.0 if all(math.isfinite(x) and x > 0 for x in (no_bid, no_ask)) else math.nan
        yes_from_no_mid = 1.0 - no_mid if math.isfinite(no_mid) else math.nan
        composite_inputs = [v for v in (yes_mid, yes_from_no_mid) if math.isfinite(v)]
        composite_mid = statistics.mean(composite_inputs) if composite_inputs else math.nan
        yes_spread = yes_ask - yes_bid if math.isfinite(yes_mid) else math.nan
        no_spread = no_ask - no_bid if math.isfinite(no_mid) else math.nan
        yes_bid_depth5, yes_ask_depth5 = yes.depth5()
        no_bid_depth5, no_ask_depth5 = no.depth5()
        quote_age_ms = max(
            (ts_ns - yes.last_update_ns) / 1_000_000.0 if yes.last_update_ns else math.inf,
            (ts_ns - no.last_update_ns) / 1_000_000.0 if no.last_update_ns else math.inf,
        )
        locked_or_crossed = (
            (math.isfinite(yes_bid) and math.isfinite(yes_ask) and yes_ask <= yes_bid)
            or (math.isfinite(no_bid) and math.isfinite(no_ask) and no_ask <= no_bid)
        )
        spread_ok = (
            math.isfinite(yes_spread) and math.isfinite(no_spread)
            and 0 < yes_spread <= max_healthy_spread
            and 0 < no_spread <= max_healthy_spread
        )
        depth_ok = (yes_bid_depth5 + yes_ask_depth5 > 0) and (no_bid_depth5 + no_ask_depth5 > 0)
        fresh_ok = quote_age_ms <= max_quote_age_ms
        healthy = bool(spread_ok and depth_ok and fresh_ok and not locked_or_crossed and math.isfinite(composite_mid))
        return {
            "yes_bid": yes_bid,
            "yes_ask": yes_ask,
            "yes_mid_direct": yes_mid,
            "yes_spread": yes_spread,
            "yes_depth5_bid_qty": yes_bid_depth5,
            "yes_depth5_ask_qty": yes_ask_depth5,
            "no_bid": no_bid,
            "no_ask": no_ask,
            "no_mid": no_mid,
            "no_spread": no_spread,
            "no_depth5_bid_qty": no_bid_depth5,
            "no_depth5_ask_qty": no_ask_depth5,
            "yes_mid_from_no": yes_from_no_mid,
            "yes_mid_composite": composite_mid,
            "complement_mid_gap": yes_mid + no_mid - 1.0 if math.isfinite(yes_mid) and math.isfinite(no_mid) else math.nan,
            "buy_both_cost": yes_ask + no_ask if math.isfinite(yes_ask) and math.isfinite(no_ask) else math.nan,
            "sell_both_credit": yes_bid + no_bid if math.isfinite(yes_bid) and math.isfinite(no_bid) else math.nan,
            "quote_age_ms": quote_age_ms,
            "locked_or_crossed": locked_or_crossed,
            "book_healthy": healthy,
            "last_trade_yes": yes.last_trade_price,
            "last_trade_no": no.last_trade_price,
        }


@dataclasses.dataclass
class ExpirySmile:
    expiry_ts: int
    years_to_expiry: float
    forward: float
    points: list[tuple[float, float]]
    strike_iv: float
    atm_iv: float
    skew_per_1k: float
    digital_call_spread: float
    digital_nd2: float


class OptionsState:
    """Read-only Deribit BTC options surface.

    For each market it locates the listed expiries that bracket the Polymarket
    settlement time, builds an IV smile around the threshold, extracts a
    risk-neutral digital via a centered call spread on each expiry, and
    interpolates in total-variance/forward space to the Polymarket settlement
    timestamp. This is the options-implied digital fair (Fair C).
    """

    def __init__(
        self,
        markets: list[MarketInfo],
        *,
        currency: str,
        strike_window: float,
        refresh_interval_sec: float,
        delta_k: float,
        max_strikes: int,
        instruments_ttl_sec: float,
    ) -> None:
        self.lock = threading.RLock()
        self.markets = markets
        self.currency = currency
        self.strike_window = strike_window
        self.refresh_interval_sec = max(2.0, refresh_interval_sec)
        self.delta_k = delta_k
        self.max_strikes = max(3, max_strikes)
        self.instruments_ttl_sec = instruments_ttl_sec
        self._instruments: list[dict[str, Any]] = []
        self._instruments_fetched = 0.0
        self.by_market: dict[str, dict[str, Any]] = {}
        self.refresh_count = 0
        self.http_calls = 0
        self.errors: list[str] = []

    def _get(self, method: str, params: dict[str, Any]) -> Any:
        url = f"{DERIBIT_API}/{method}?{urllib.parse.urlencode(params)}"
        data = http_json(url, timeout_s=10)
        self.http_calls += 1
        if not isinstance(data, dict) or "result" not in data:
            raise RuntimeError(f"deribit {method} returned no result")
        return data["result"]

    def _ensure_instruments(self) -> None:
        now = time.time()
        if self._instruments and (now - self._instruments_fetched) < self.instruments_ttl_sec:
            return
        result = self._get(
            "public/get_instruments",
            {"currency": self.currency, "kind": "option", "expired": "false"},
        )
        self._instruments = [i for i in result if isinstance(i, dict)]
        self._instruments_fetched = now

    def _expiries(self) -> list[int]:
        return sorted(
            {
                int(i["expiration_timestamp"]) // 1000
                for i in self._instruments
                if i.get("expiration_timestamp")
            }
        )

    def _bracketing_expiries(self, target_unix: int) -> list[int]:
        exps = self._expiries()
        if not exps:
            return []
        lo = max((e for e in exps if e <= target_unix), default=exps[0])
        hi = min((e for e in exps if e >= target_unix), default=exps[-1])
        out: list[int] = []
        for e in (lo, hi):
            if e not in out:
                out.append(e)
        return out

    def _build_expiry_smile(self, expiry_unix: int, strike: float, now: float) -> ExpirySmile | None:
        calls = [
            i
            for i in self._instruments
            if int(i.get("expiration_timestamp", 0)) // 1000 == expiry_unix
            and str(i.get("instrument_name", "")).endswith("-C")
            and math.isfinite(number(i.get("strike")))
            and abs(number(i.get("strike")) - strike) <= self.strike_window
        ]
        calls.sort(key=lambda i: abs(number(i.get("strike")) - strike))
        calls = calls[: self.max_strikes]
        if not calls:
            return None
        points: list[tuple[float, float]] = []
        forwards: list[float] = []
        for inst in calls:
            try:
                ob = self._get("public/get_order_book", {"instrument_name": inst["instrument_name"]})
            except Exception as exc:  # noqa: BLE001
                self.errors.append(f"orderbook {inst.get('instrument_name')}: {exc}")
                continue
            iv = number(ob.get("mark_iv"))
            forward = number(ob.get("underlying_price"))
            k = number(inst.get("strike"))
            if math.isfinite(iv) and iv > 0 and math.isfinite(k):
                points.append((k, iv / 100.0))
            if math.isfinite(forward) and forward > 0:
                forwards.append(forward)
        if len(points) < 2 or not forwards:
            return None
        forward = statistics.median(forwards)
        years = max(0.0, (expiry_unix - now) / SECONDS_PER_YEAR)
        dk = self.delta_k
        iv_strike = interp_smile(points, strike)
        iv_lo = interp_smile(points, strike - dk)
        iv_hi = interp_smile(points, strike + dk)
        call_lo = bs_call_undiscounted(forward, strike - dk, years, iv_lo)
        call_hi = bs_call_undiscounted(forward, strike + dk, years, iv_hi)
        digital_cs = math.nan
        if math.isfinite(call_lo) and math.isfinite(call_hi) and dk > 0:
            digital_cs = min(1.0, max(0.0, (call_lo - call_hi) / (2.0 * dk)))
        skew = math.nan
        if math.isfinite(iv_hi) and math.isfinite(iv_lo) and dk > 0:
            skew = (iv_hi - iv_lo) / (2.0 * dk) * 1000.0
        return ExpirySmile(
            expiry_ts=expiry_unix,
            years_to_expiry=years,
            forward=forward,
            points=points,
            strike_iv=iv_strike,
            atm_iv=interp_smile(points, forward),
            skew_per_1k=skew,
            digital_call_spread=digital_cs,
            digital_nd2=bs_digital_call_nd2(forward, strike, years, iv_strike),
        )

    def _interp_target(
        self,
        lo: ExpirySmile | None,
        hi: ExpirySmile | None,
        strike: float,
        target_unix: int,
        now: float,
    ) -> dict[str, float] | None:
        target_years = max(0.0, (target_unix - now) / SECONDS_PER_YEAR)
        if target_years <= 0:
            return None
        dk = self.delta_k
        ks = (strike - dk, strike, strike + dk)
        if lo is None and hi is None:
            return None
        if lo is None or hi is None or lo.expiry_ts == hi.expiry_ts:
            base = lo or hi
            assert base is not None
            forward = base.forward
            iv_at = {k: interp_smile(base.points, k) for k in ks}
            strike_iv = iv_at[strike]
            atm_iv = base.atm_iv
            skew = base.skew_per_1k
        else:
            t_lo, t_hi = lo.years_to_expiry, hi.years_to_expiry
            span = t_hi - t_lo
            frac = (target_years - t_lo) / span if span > 0 else 0.0
            frac = min(1.0, max(0.0, frac))
            forward = math.exp(math.log(lo.forward) + frac * (math.log(hi.forward) - math.log(lo.forward)))
            iv_at: dict[float, float] = {}
            for k in ks:
                iv_lo = interp_smile(lo.points, k)
                iv_hi = interp_smile(hi.points, k)
                w_lo = iv_lo * iv_lo * t_lo
                w_hi = iv_hi * iv_hi * t_hi
                w_t = w_lo + frac * (w_hi - w_lo)
                iv_at[k] = math.sqrt(w_t / target_years) if w_t > 0 else math.nan
            strike_iv = iv_at[strike]
            atm_iv = math.nan
            skew = (
                (iv_at[strike + dk] - iv_at[strike - dk]) / (2.0 * dk) * 1000.0
                if math.isfinite(iv_at[strike + dk]) and math.isfinite(iv_at[strike - dk])
                else math.nan
            )
        call_lo = bs_call_undiscounted(forward, strike - dk, target_years, iv_at[strike - dk])
        call_hi = bs_call_undiscounted(forward, strike + dk, target_years, iv_at[strike + dk])
        digital = math.nan
        if math.isfinite(call_lo) and math.isfinite(call_hi) and dk > 0:
            digital = min(1.0, max(0.0, (call_lo - call_hi) / (2.0 * dk)))
        return {
            "fair": digital,
            "fair_nd2": bs_digital_call_nd2(forward, strike, target_years, strike_iv),
            "forward": forward,
            "strike_iv": strike_iv,
            "atm_iv": atm_iv,
            "skew_per_1k": skew,
            "target_years": target_years,
        }

    def refresh(self) -> None:
        try:
            self._ensure_instruments()
        except Exception as exc:  # noqa: BLE001
            self.errors.append(f"instruments: {exc}")
            return
        now = time.time()
        result: dict[str, dict[str, Any]] = {}
        for market in self.markets:
            target = int(market.end_ts)
            if not target:
                continue
            smiles: list[ExpirySmile] = []
            for expiry in self._bracketing_expiries(target):
                try:
                    smile = self._build_expiry_smile(expiry, market.threshold, now)
                except Exception as exc:  # noqa: BLE001
                    self.errors.append(f"smile {expiry}: {exc}")
                    smile = None
                if smile is not None:
                    smiles.append(smile)
            lo = smiles[0] if smiles else None
            hi = smiles[-1] if len(smiles) > 1 else (smiles[0] if smiles else None)
            interp = self._interp_target(lo, hi, market.threshold, target, now) if smiles else None
            result[market.market_id] = {
                "options_surface_ok": bool(interp and math.isfinite(interp.get("fair", math.nan))),
                "options_fair": interp.get("fair", math.nan) if interp else math.nan,
                "options_fair_nd2": interp.get("fair_nd2", math.nan) if interp else math.nan,
                "options_forward": interp.get("forward", math.nan) if interp else math.nan,
                "options_strike_iv": interp.get("strike_iv", math.nan) if interp else math.nan,
                "options_atm_iv": interp.get("atm_iv", math.nan) if interp else math.nan,
                "options_skew_per_1k": interp.get("skew_per_1k", math.nan) if interp else math.nan,
                "options_target_years": interp.get("target_years", math.nan) if interp else math.nan,
                "options_expiry_lo_ts": lo.expiry_ts if lo else 0,
                "options_expiry_hi_ts": hi.expiry_ts if hi else 0,
                "settlement_gap_hours_lo": (lo.expiry_ts - target) / 3600.0 if lo else math.nan,
                "settlement_gap_hours_hi": (hi.expiry_ts - target) / 3600.0 if hi else math.nan,
                "options_n_smile_points_lo": len(lo.points) if lo else 0,
                "options_n_smile_points_hi": len(hi.points) if hi else 0,
                "options_digital_cs_lo": lo.digital_call_spread if lo else math.nan,
                "options_digital_cs_hi": hi.digital_call_spread if hi else math.nan,
                "options_strike_iv_lo": lo.strike_iv if lo else math.nan,
                "options_strike_iv_hi": hi.strike_iv if hi else math.nan,
            }
        with self.lock:
            self.by_market = result
            self.refresh_count += 1

    def loop(self, stop: threading.Event) -> None:
        while not stop.is_set():
            self.refresh()
            stop.wait(self.refresh_interval_sec)

    def snapshot(self, market_id: str) -> dict[str, Any]:
        with self.lock:
            return dict(self.by_market.get(market_id, {}))


def ws_loop(
    *,
    name: str,
    url: str,
    stop: threading.Event,
    on_message,
    on_open_send: list[dict[str, Any]] | None = None,
    errors: list[str] | None = None,
) -> None:
    sends = on_open_send or []
    while not stop.is_set():
        ws = None
        try:
            ws = websocket.create_connection(url, timeout=10)
            for message in sends:
                ws.send(json.dumps(message, separators=(",", ":")))
            ws.settimeout(1)
            while not stop.is_set():
                try:
                    raw = ws.recv()
                except websocket.WebSocketTimeoutException:
                    continue
                if not raw:
                    continue
                try:
                    payload = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                on_message(payload)
        except Exception as exc:  # noqa: BLE001
            if errors is not None:
                errors.append(f"{name}: {exc}")
            if not stop.is_set():
                time.sleep(1.0)
        finally:
            if ws is not None:
                try:
                    ws.close()
                except Exception:  # noqa: BLE001
                    pass


class BasisTracker:
    def __init__(self, *, tau_sec: float) -> None:
        self.tau_sec = max(1.0, tau_sec)
        self.value = math.nan
        self.last_ts_ns = 0
        self.updates = 0

    def update(self, ts_ns: int, deviation: float, *, healthy: bool) -> float:
        if not healthy or not math.isfinite(deviation):
            return self.value
        if not math.isfinite(self.value):
            self.value = deviation
            self.last_ts_ns = ts_ns
            self.updates += 1
            return self.value
        dt_sec = max(0.0, (ts_ns - self.last_ts_ns) / 1_000_000_000.0)
        alpha = 1.0 - math.exp(-dt_sec / self.tau_sec)
        self.value = alpha * deviation + (1.0 - alpha) * self.value
        self.last_ts_ns = ts_ns
        self.updates += 1
        return self.value


def make_sample_fieldnames(markets: list[MarketInfo]) -> list[str]:
    base = [
        "sample_index",
        "ts_ns",
        "unix_seconds",
        "iso_utc",
        "rel_ms",
        "spot_bid",
        "spot_ask",
        "spot_mid",
        "spot_bid_qty",
        "spot_ask_qty",
        "spot_depth5_bid_qty",
        "spot_depth5_ask_qty",
        "spot_depth5_imbalance",
        "ofi_100ms",
        "ofi_500ms",
        "realized_vol_annual_bps",
    ]
    market_fields = []
    per_market = [
        "threshold",
        "seconds_to_expiry",
        "vol_annual_bps_used",
        "raw_fair_yes",
        "raw_fair_no",
        "raw_fair_yes_logit",
        "yes_bid",
        "yes_ask",
        "yes_mid_direct",
        "yes_spread",
        "yes_depth5_bid_qty",
        "yes_depth5_ask_qty",
        "no_bid",
        "no_ask",
        "no_mid",
        "no_spread",
        "no_depth5_bid_qty",
        "no_depth5_ask_qty",
        "yes_mid_from_no",
        "yes_mid_composite",
        "complement_mid_gap",
        "buy_both_cost",
        "sell_both_credit",
        "quote_age_ms",
        "locked_or_crossed",
        "book_healthy",
        "deviation_direct",
        "deviation_from_no",
        "deviation_composite",
        "deviation_composite_logit",
        "basis_ewma",
        "basis_residual",
        "basis_updates",
        "fixed_vol_fair",
        "realized_vol_fair",
        "options_fair",
        "options_fair_nd2",
        "options_forward",
        "options_strike_iv",
        "options_atm_iv",
        "options_skew_per_1k",
        "options_surface_ok",
        "options_expiry_lo_ts",
        "options_expiry_hi_ts",
        "settlement_gap_hours_lo",
        "settlement_gap_hours_hi",
        "options_digital_cs_lo",
        "options_digital_cs_hi",
        "deviation_fixed",
        "deviation_realized",
        "deviation_options",
        "deviation_options_logit",
        "expected_poly_basis",
        "expected_poly_fair_yes",
        "deviation_vs_expected_poly_fair",
        "basis_ewma_options",
        "basis_residual_options",
        "basis_updates_options",
    ]
    for idx, _ in enumerate(markets):
        prefix = f"m{idx}_"
        market_fields.extend(prefix + field for field in per_market)
    return base + market_fields


def row_market_prefix(index: int) -> str:
    return f"m{index}_"


def compute_row(
    *,
    idx: int,
    start_wall: float,
    ts_ns: int,
    oracle: OracleState,
    poly: PolyState,
    options: OptionsState | None,
    markets: list[MarketInfo],
    basis_trackers: dict[str, dict[str, BasisTracker]],
    args: argparse.Namespace,
) -> dict[str, Any]:
    ts_seconds = ts_ns / 1_000_000_000.0
    oracle_snapshot = oracle.snapshot(ts_ns)
    spot_mid = float(oracle_snapshot["spot_mid"])
    realized_vol = float(oracle_snapshot["realized_vol_annual_bps"])
    row: dict[str, Any] = {
        "sample_index": idx,
        "ts_ns": ts_ns,
        "unix_seconds": ts_seconds,
        "iso_utc": iso_utc_from_unix(ts_seconds),
        "rel_ms": (ts_seconds - start_wall) * 1000.0,
        **oracle_snapshot,
    }
    for market_idx, market in enumerate(markets):
        prefix = row_market_prefix(market_idx)
        seconds_to_expiry = max(0.0, float(market.end_ts) - ts_seconds) if market.end_ts else math.nan

        # Fair A: fixed-vol lognormal digital (our assumed vol).
        fixed_vol_fair = digital_call_probability(
            spot=spot_mid,
            strike=market.threshold,
            seconds_to_expiry=seconds_to_expiry,
            vol_annual_bps=args.vol_annual_bps,
            drift_annual_bps=args.drift_annual_bps,
        )
        # Fair B: realized-vol lognormal digital (past window volatility).
        realized_vol_fair = math.nan
        if math.isfinite(realized_vol) and realized_vol > 0:
            realized_vol_fair = digital_call_probability(
                spot=spot_mid,
                strike=market.threshold,
                seconds_to_expiry=seconds_to_expiry,
                vol_annual_bps=realized_vol,
                drift_annual_bps=args.drift_annual_bps,
            )
        # Fair C: options-implied risk-neutral digital from the Deribit smile.
        options_snap = options.snapshot(market.market_id) if options is not None else {}
        options_fair = float(options_snap.get("options_fair", math.nan))
        options_surface_ok = bool(options_snap.get("options_surface_ok", False))

        # raw_fair_yes preserves the legacy single-fair semantics (driven by --fair-vol-mode).
        vol_used = args.vol_annual_bps
        fair_yes = fixed_vol_fair
        if args.fair_vol_mode == "realized" and math.isfinite(realized_vol_fair):
            vol_used = realized_vol
            fair_yes = realized_vol_fair
        fair_no = 1.0 - fair_yes if math.isfinite(fair_yes) else math.nan

        book = poly.snapshot_market(
            market,
            ts_ns,
            max_healthy_spread=args.max_healthy_spread,
            max_quote_age_ms=args.max_quote_age_ms,
        )
        yes_mid_direct = float(book["yes_mid_direct"])
        yes_mid_from_no = float(book["yes_mid_from_no"])
        yes_mid_composite = float(book["yes_mid_composite"])
        book_healthy = bool(book["book_healthy"])

        def _dev(reference: float) -> float:
            return (
                yes_mid_composite - reference
                if math.isfinite(yes_mid_composite) and math.isfinite(reference)
                else math.nan
            )

        deviation_direct = yes_mid_direct - fair_yes if math.isfinite(yes_mid_direct) and math.isfinite(fair_yes) else math.nan
        deviation_from_no = yes_mid_from_no - fair_yes if math.isfinite(yes_mid_from_no) and math.isfinite(fair_yes) else math.nan
        deviation_composite = _dev(fair_yes)
        deviation_composite_logit = logit(yes_mid_composite) - logit(fair_yes) if math.isfinite(yes_mid_composite) and math.isfinite(fair_yes) else math.nan

        deviation_fixed = _dev(fixed_vol_fair)
        deviation_realized = _dev(realized_vol_fair)
        deviation_options = _dev(options_fair)
        deviation_options_logit = (
            logit(yes_mid_composite) - logit(options_fair)
            if math.isfinite(yes_mid_composite) and math.isfinite(options_fair)
            else math.nan
        )
        expected_poly_basis = args.expected_poly_basis
        expected_poly_fair_yes = (
            clamp_prob(options_fair + expected_poly_basis)
            if math.isfinite(options_fair) and math.isfinite(expected_poly_basis)
            else math.nan
        )
        deviation_vs_expected_poly_fair = (
            yes_mid_composite - expected_poly_fair_yes
            if math.isfinite(yes_mid_composite) and math.isfinite(expected_poly_fair_yes)
            else math.nan
        )

        trackers = basis_trackers[market.market_id]
        basis = trackers["fixed"].update(ts_ns, deviation_composite, healthy=book_healthy)
        residual = deviation_composite - basis if math.isfinite(deviation_composite) and math.isfinite(basis) else math.nan
        basis_options = trackers["options"].update(
            ts_ns, deviation_options, healthy=book_healthy and options_surface_ok
        )
        residual_options = (
            deviation_options - basis_options
            if math.isfinite(deviation_options) and math.isfinite(basis_options)
            else math.nan
        )

        market_values = {
            "threshold": market.threshold,
            "seconds_to_expiry": seconds_to_expiry,
            "vol_annual_bps_used": vol_used,
            "raw_fair_yes": fair_yes,
            "raw_fair_no": fair_no,
            "raw_fair_yes_logit": logit(fair_yes),
            **book,
            "deviation_direct": deviation_direct,
            "deviation_from_no": deviation_from_no,
            "deviation_composite": deviation_composite,
            "deviation_composite_logit": deviation_composite_logit,
            "basis_ewma": basis,
            "basis_residual": residual,
            "basis_updates": trackers["fixed"].updates,
            "fixed_vol_fair": fixed_vol_fair,
            "realized_vol_fair": realized_vol_fair,
            "options_fair": options_fair,
            "options_fair_nd2": options_snap.get("options_fair_nd2", math.nan),
            "options_forward": options_snap.get("options_forward", math.nan),
            "options_strike_iv": options_snap.get("options_strike_iv", math.nan),
            "options_atm_iv": options_snap.get("options_atm_iv", math.nan),
            "options_skew_per_1k": options_snap.get("options_skew_per_1k", math.nan),
            "options_surface_ok": options_surface_ok,
            "options_expiry_lo_ts": options_snap.get("options_expiry_lo_ts", 0),
            "options_expiry_hi_ts": options_snap.get("options_expiry_hi_ts", 0),
            "settlement_gap_hours_lo": options_snap.get("settlement_gap_hours_lo", math.nan),
            "settlement_gap_hours_hi": options_snap.get("settlement_gap_hours_hi", math.nan),
            "options_digital_cs_lo": options_snap.get("options_digital_cs_lo", math.nan),
            "options_digital_cs_hi": options_snap.get("options_digital_cs_hi", math.nan),
            "deviation_fixed": deviation_fixed,
            "deviation_realized": deviation_realized,
            "deviation_options": deviation_options,
            "deviation_options_logit": deviation_options_logit,
            "expected_poly_basis": expected_poly_basis,
            "expected_poly_fair_yes": expected_poly_fair_yes,
            "deviation_vs_expected_poly_fair": deviation_vs_expected_poly_fair,
            "basis_ewma_options": basis_options,
            "basis_residual_options": residual_options,
            "basis_updates_options": trackers["options"].updates,
        }
        for key, value in market_values.items():
            row[prefix + key] = value
    return row


def summarize_rows(rows: list[dict[str, Any]], markets: list[MarketInfo]) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "sample_count": len(rows),
        "first_ts_ns": rows[0]["ts_ns"] if rows else 0,
        "last_ts_ns": rows[-1]["ts_ns"] if rows else 0,
        "first_iso_utc": rows[0]["iso_utc"] if rows else "",
        "last_iso_utc": rows[-1]["iso_utc"] if rows else "",
        "spot_mid_first": rows[0].get("spot_mid", math.nan) if rows else math.nan,
        "spot_mid_last": rows[-1].get("spot_mid", math.nan) if rows else math.nan,
        "realized_vol_annual_bps_last": rows[-1].get("realized_vol_annual_bps", math.nan) if rows else math.nan,
        "markets": [],
    }
    for idx, market in enumerate(markets):
        prefix = row_market_prefix(idx)
        healthy_rows = [r for r in rows if bool(r.get(prefix + "book_healthy", False))]
        deviations = [float(r.get(prefix + "deviation_composite", math.nan)) for r in healthy_rows]
        residuals = [float(r.get(prefix + "basis_residual", math.nan)) for r in healthy_rows]
        direct_devs = [float(r.get(prefix + "deviation_direct", math.nan)) for r in healthy_rows]
        spreads_yes = [float(r.get(prefix + "yes_spread", math.nan)) for r in healthy_rows]
        spreads_no = [float(r.get(prefix + "no_spread", math.nan)) for r in healthy_rows]
        complement_gaps = [float(r.get(prefix + "complement_mid_gap", math.nan)) for r in healthy_rows]
        fair = np.array([float(r.get(prefix + "raw_fair_yes", math.nan)) for r in healthy_rows], dtype=float)
        mid = np.array([float(r.get(prefix + "yes_mid_composite", math.nan)) for r in healthy_rows], dtype=float)
        corr_level = math.nan
        corr_return = math.nan
        mask = np.isfinite(fair) & np.isfinite(mid)
        if int(mask.sum()) >= 10 and np.std(fair[mask]) > 0 and np.std(mid[mask]) > 0:
            corr_level = float(np.corrcoef(fair[mask], mid[mask])[0, 1])
        if int(mask.sum()) >= 11:
            fair_ret = np.diff(fair[mask])
            mid_ret = np.diff(mid[mask])
            ret_mask = np.isfinite(fair_ret) & np.isfinite(mid_ret)
            if int(ret_mask.sum()) >= 10 and np.std(fair_ret[ret_mask]) > 0 and np.std(mid_ret[ret_mask]) > 0:
                corr_return = float(np.corrcoef(fair_ret[ret_mask], mid_ret[ret_mask])[0, 1])

        options_rows = [r for r in healthy_rows if bool(r.get(prefix + "options_surface_ok", False))]
        dev_fixed = [float(r.get(prefix + "deviation_fixed", math.nan)) for r in healthy_rows]
        dev_realized = [float(r.get(prefix + "deviation_realized", math.nan)) for r in healthy_rows]
        dev_options = [float(r.get(prefix + "deviation_options", math.nan)) for r in options_rows]
        dev_expected_poly_fair = [float(r.get(prefix + "deviation_vs_expected_poly_fair", math.nan)) for r in options_rows]
        residuals_options = [float(r.get(prefix + "basis_residual_options", math.nan)) for r in options_rows]
        opt_fair = np.array([float(r.get(prefix + "options_fair", math.nan)) for r in options_rows], dtype=float)
        opt_mid = np.array([float(r.get(prefix + "yes_mid_composite", math.nan)) for r in options_rows], dtype=float)
        corr_level_options = math.nan
        opt_mask = np.isfinite(opt_fair) & np.isfinite(opt_mid)
        if int(opt_mask.sum()) >= 10 and np.std(opt_fair[opt_mask]) > 0 and np.std(opt_mid[opt_mask]) > 0:
            corr_level_options = float(np.corrcoef(opt_fair[opt_mask], opt_mid[opt_mask])[0, 1])

        def _fair_block(label: str, devs: list[float], reference_field: str) -> dict[str, Any]:
            clean = [x for x in devs if math.isfinite(x)]
            return {
                f"{label}_fair_last": rows[-1].get(prefix + reference_field, math.nan) if rows else math.nan,
                f"deviation_{label}_mean": safe_mean(devs),
                f"deviation_{label}_median": safe_median(devs),
                f"deviation_{label}_p05": percentile(devs, 0.05),
                f"deviation_{label}_p95": percentile(devs, 0.95),
                f"deviation_{label}_abs_p95": percentile([abs(x) for x in clean], 0.95),
            }

        market_summary = {
            "market_index": idx,
            "slug": market.slug,
            "question": market.question,
            "threshold": market.threshold,
            "end_ts": market.end_ts,
            "end_iso_utc": iso_utc_from_unix(market.end_ts) if market.end_ts else "",
            "healthy_sample_count": len(healthy_rows),
            "healthy_ratio": len(healthy_rows) / len(rows) if rows else math.nan,
            "options_sample_count": len(options_rows),
            "fair_first": rows[0].get(prefix + "raw_fair_yes", math.nan) if rows else math.nan,
            "fair_last": rows[-1].get(prefix + "raw_fair_yes", math.nan) if rows else math.nan,
            "poly_mid_first": rows[0].get(prefix + "yes_mid_composite", math.nan) if rows else math.nan,
            "poly_mid_last": rows[-1].get(prefix + "yes_mid_composite", math.nan) if rows else math.nan,
            "basis_ewma_last": rows[-1].get(prefix + "basis_ewma", math.nan) if rows else math.nan,
            "basis_ewma_options_last": rows[-1].get(prefix + "basis_ewma_options", math.nan) if rows else math.nan,
            "deviation_mean": safe_mean(deviations),
            "deviation_median": safe_median(deviations),
            "deviation_p05": percentile(deviations, 0.05),
            "deviation_p95": percentile(deviations, 0.95),
            "deviation_abs_p95": percentile([abs(x) for x in deviations], 0.95),
            "deviation_direct_mean": safe_mean(direct_devs),
            "residual_mean": safe_mean(residuals),
            "residual_abs_p95": percentile([abs(x) for x in residuals], 0.95),
            "residual_options_mean": safe_mean(residuals_options),
            "residual_options_abs_p95": percentile([abs(x) for x in residuals_options if math.isfinite(x)], 0.95),
            "yes_spread_median": safe_median(spreads_yes),
            "no_spread_median": safe_median(spreads_no),
            "complement_mid_gap_mean": safe_mean(complement_gaps),
            "corr_level_fair_vs_poly_mid": corr_level,
            "corr_return_fair_vs_poly_mid": corr_return,
            "corr_level_options_fair_vs_poly_mid": corr_level_options,
            "options_forward_last": rows[-1].get(prefix + "options_forward", math.nan) if rows else math.nan,
            "options_strike_iv_last": rows[-1].get(prefix + "options_strike_iv", math.nan) if rows else math.nan,
            "options_skew_per_1k_last": rows[-1].get(prefix + "options_skew_per_1k", math.nan) if rows else math.nan,
            "settlement_gap_hours_lo": rows[-1].get(prefix + "settlement_gap_hours_lo", math.nan) if rows else math.nan,
            "settlement_gap_hours_hi": rows[-1].get(prefix + "settlement_gap_hours_hi", math.nan) if rows else math.nan,
        }
        market_summary.update(_fair_block("fixed", dev_fixed, "fixed_vol_fair"))
        market_summary.update(_fair_block("realized", dev_realized, "realized_vol_fair"))
        market_summary.update(_fair_block("options", dev_options, "options_fair"))
        market_summary["expected_poly_basis"] = rows[-1].get(prefix + "expected_poly_basis", math.nan) if rows else math.nan
        market_summary["expected_poly_fair_yes_last"] = rows[-1].get(prefix + "expected_poly_fair_yes", math.nan) if rows else math.nan
        market_summary["deviation_vs_expected_poly_fair_mean"] = safe_mean(dev_expected_poly_fair)
        market_summary["deviation_vs_expected_poly_fair_median"] = safe_median(dev_expected_poly_fair)
        market_summary["deviation_vs_expected_poly_fair_abs_p95"] = percentile([abs(x) for x in dev_expected_poly_fair if math.isfinite(x)], 0.95)
        summary["markets"].append(market_summary)
    return summary


def write_csv_header_if_needed(path: Path, fieldnames: list[str]) -> None:
    if not path.exists():
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
            writer.writeheader()


def append_csv_row(path: Path, fieldnames: list[str], row: dict[str, Any]) -> None:
    with path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writerow(row)


def write_stage_summary(
    *,
    out_dir: Path,
    stage_idx: int,
    all_rows: list[dict[str, Any]],
    stage_rows: list[dict[str, Any]],
    markets: list[MarketInfo],
    metadata: dict[str, Any],
) -> None:
    stages_dir = out_dir / "stages"
    stages_dir.mkdir(exist_ok=True)
    payload = {
        "stage_index": stage_idx,
        "written_at_utc": iso_utc_from_unix(time.time()),
        "metadata": metadata,
        "stage_summary": summarize_rows(stage_rows, markets),
        "cumulative_summary": summarize_rows(all_rows, markets),
    }
    file_path = stages_dir / f"stage_{stage_idx:03d}_{utc_stamp()}.json"
    file_path.write_text(json.dumps(payload, indent=2, allow_nan=True) + "\n", encoding="utf-8")
    with (stages_dir / "stage_summaries.jsonl").open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, allow_nan=True) + "\n")


def make_plots(out_dir: Path, rows: list[dict[str, Any]], markets: list[MarketInfo]) -> None:
    if not rows:
        return
    rel_s = np.array([float(row["rel_ms"]) / 1000.0 for row in rows], dtype=float)
    for idx, market in enumerate(markets):
        prefix = row_market_prefix(idx)
        fixed_fair = np.array([float(row.get(prefix + "fixed_vol_fair", math.nan)) for row in rows], dtype=float)
        realized_fair = np.array([float(row.get(prefix + "realized_vol_fair", math.nan)) for row in rows], dtype=float)
        options_fair = np.array([float(row.get(prefix + "options_fair", math.nan)) for row in rows], dtype=float)
        expected_poly_fair = np.array([float(row.get(prefix + "expected_poly_fair_yes", math.nan)) for row in rows], dtype=float)
        mid = np.array([float(row.get(prefix + "yes_mid_composite", math.nan)) for row in rows], dtype=float)
        direct = np.array([float(row.get(prefix + "yes_mid_direct", math.nan)) for row in rows], dtype=float)
        from_no = np.array([float(row.get(prefix + "yes_mid_from_no", math.nan)) for row in rows], dtype=float)
        dev_fixed = np.array([float(row.get(prefix + "deviation_fixed", math.nan)) for row in rows], dtype=float)
        dev_realized = np.array([float(row.get(prefix + "deviation_realized", math.nan)) for row in rows], dtype=float)
        dev_options = np.array([float(row.get(prefix + "deviation_options", math.nan)) for row in rows], dtype=float)
        dev_expected = np.array([float(row.get(prefix + "deviation_vs_expected_poly_fair", math.nan)) for row in rows], dtype=float)
        basis_options = np.array([float(row.get(prefix + "basis_ewma_options", math.nan)) for row in rows], dtype=float)
        residual_options = np.array([float(row.get(prefix + "basis_residual_options", math.nan)) for row in rows], dtype=float)

        fig, axes = plt.subplots(3, 1, figsize=(12, 11), sharex=True)
        axes[0].plot(rel_s, mid, label="Poly composite YES mid", linewidth=1.4, color="black")
        axes[0].plot(rel_s, options_fair, label="Fair C: options-implied digital", linewidth=1.3, color="tab:red")
        axes[0].plot(rel_s, expected_poly_fair, label="Options fair + expected Poly basis", linewidth=1.1, color="tab:purple", alpha=0.9)
        axes[0].plot(rel_s, realized_fair, label="Fair B: realized-vol fair", linewidth=1.0, color="tab:green", alpha=0.85)
        axes[0].plot(rel_s, fixed_fair, label="Fair A: fixed-vol fair", linewidth=1.0, color="tab:blue", alpha=0.85)
        axes[0].plot(rel_s, direct, label="Poly direct YES mid", linewidth=0.7, alpha=0.5)
        axes[0].plot(rel_s, from_no, label="Poly YES from NO mid", linewidth=0.7, alpha=0.5)
        axes[0].set_ylabel("Probability")
        axes[0].legend(loc="best", fontsize=8)
        axes[0].grid(True, alpha=0.25)

        axes[1].plot(rel_s, dev_options, label="D_options = Poly - options fair", linewidth=1.3, color="tab:red")
        axes[1].plot(rel_s, dev_expected, label="Poly - (options fair + expected basis)", linewidth=1.1, color="tab:purple", alpha=0.9)
        axes[1].plot(rel_s, dev_realized, label="D_realized", linewidth=1.0, color="tab:green", alpha=0.8)
        axes[1].plot(rel_s, dev_fixed, label="D_fixed", linewidth=1.0, color="tab:blue", alpha=0.8)
        axes[1].axhline(0, color="black", linewidth=0.8)
        axes[1].set_ylabel("Poly - fair")
        axes[1].legend(loc="best", fontsize=8)
        axes[1].grid(True, alpha=0.25)

        axes[2].plot(rel_s, basis_options, label="options basis EWMA", linewidth=1.1, color="tab:purple")
        axes[2].plot(rel_s, residual_options, label="options basis residual", linewidth=1.0, color="tab:orange")
        axes[2].axhline(0, color="black", linewidth=0.8)
        axes[2].set_xlabel("Seconds from capture start")
        axes[2].set_ylabel("Options basis")
        axes[2].legend(loc="best", fontsize=8)
        axes[2].grid(True, alpha=0.25)

        fig.suptitle(f"{market.question} | K={market.threshold:g} | three-layer fair")
        fig.tight_layout()
        fig.savefig(out_dir / f"market_{idx}_fair_vs_poly.png", dpi=140)
        plt.close(fig)


def write_report(out_dir: Path, summary: dict[str, Any]) -> None:
    lines = [
        "# Polymarket BTC Above Fair-Value Research",
        "",
        "Read-only capture. No orders, private keys, or trading endpoints were used.",
        "",
        "## Three-layer fair framework",
        "",
        "- `fixed_vol_fair` (Fair A): lognormal digital `P(BTC_T > K)` with an assumed fixed vol.",
        "- `realized_vol_fair` (Fair B): same model but using the rolling realized vol of BTC.",
        "- `options_fair` (Fair C, primary benchmark): risk-neutral digital extracted from the",
        "  Deribit BTC options IV smile via a centered call spread `[C(K-dK) - C(K+dK)] / (2 dK)`,",
        "  using the per-expiry forward and interpolated in total-variance/forward space to the",
        "  Polymarket settlement timestamp. `options_fair_nd2` is the smile-free `N(d2)` reference.",
        "- `yes_mid_composite`: average of direct YES mid and `1 - NO mid` when both are healthy.",
        "- `deviation_fixed / deviation_realized / deviation_options`: `Poly composite mid - fair`.",
        "  `deviation_options` is the prediction-market basis vs the options-implied distribution.",
        "- `expected_poly_fair_yes`: `options_fair + expected_poly_basis`, clipped to probability bounds.",
        "- `deviation_vs_expected_poly_fair`: Poly composite mid minus the expected-basis-adjusted fair.",
        "- `settlement_gap_hours_lo/hi`: hours between each bracketing option expiry and the",
        "  Polymarket settlement time (settlement-basis mismatch; Fair C is time-interpolated).",
        "- `basis_ewma_options` / `basis_residual_options`: EWMA of `deviation_options` and its residual.",
        "",
        "## Capture",
        "",
        f"- Output directory: `{summary['out_dir']}`",
        f"- Samples: {summary['final_summary']['sample_count']}",
        f"- Started: {summary['final_summary']['first_iso_utc']}",
        f"- Ended: {summary['final_summary']['last_iso_utc']}",
        f"- Sample interval ms: {summary['metadata']['sample_interval_ms']}",
        f"- Stage interval seconds: {summary['metadata']['stage_interval_seconds']}",
        f"- Fair volatility mode: `{summary['metadata']['fair_vol_mode']}`",
        "",
        "## Markets",
        "",
    ]
    def fmt(value: Any, spec: str = ".6f") -> str:
        try:
            number_value = float(value)
        except (TypeError, ValueError):
            return "nan"
        return f"{number_value:{spec}}" if math.isfinite(number_value) else "nan"

    for market_summary in summary["final_summary"].get("markets", []):
        lines += [
            f"### {market_summary['question']}",
            "",
            f"- Threshold: {market_summary['threshold']:g}",
            f"- End (Poly settlement): {market_summary['end_iso_utc']}",
            f"- Healthy samples: {market_summary['healthy_sample_count']} ({fmt(market_summary['healthy_ratio'], '.2%')})",
            f"- Options-surface samples: {market_summary.get('options_sample_count', 0)}",
            f"- Poly composite mid first/last: {fmt(market_summary['poly_mid_first'])} / {fmt(market_summary['poly_mid_last'])}",
            "",
            "Three-layer fair comparison (Poly - fair, mean / median / p95-abs):",
            "",
            f"- Fair A fixed-vol   last={fmt(market_summary.get('fixed_fair_last'))}  "
            f"D_fixed={fmt(market_summary.get('deviation_fixed_mean'))} / "
            f"{fmt(market_summary.get('deviation_fixed_median'))} / {fmt(market_summary.get('deviation_fixed_abs_p95'))}",
            f"- Fair B realized-vol last={fmt(market_summary.get('realized_fair_last'))}  "
            f"D_realized={fmt(market_summary.get('deviation_realized_mean'))} / "
            f"{fmt(market_summary.get('deviation_realized_median'))} / {fmt(market_summary.get('deviation_realized_abs_p95'))}",
            f"- Fair C options     last={fmt(market_summary.get('options_fair_last'))}  "
            f"**D_options={fmt(market_summary.get('deviation_options_mean'))}** / "
            f"{fmt(market_summary.get('deviation_options_median'))} / {fmt(market_summary.get('deviation_options_abs_p95'))}",
            f"- Expected Poly fair last={fmt(market_summary.get('expected_poly_fair_yes_last'))} "
            f"using basis={fmt(market_summary.get('expected_poly_basis'))}; "
            f"residual mean/median/p95abs={fmt(market_summary.get('deviation_vs_expected_poly_fair_mean'))} / "
            f"{fmt(market_summary.get('deviation_vs_expected_poly_fair_median'))} / "
            f"{fmt(market_summary.get('deviation_vs_expected_poly_fair_abs_p95'))}",
            "",
            f"- Options forward last: {fmt(market_summary.get('options_forward_last'), '.1f')}",
            f"- Options strike IV last: {fmt(market_summary.get('options_strike_iv_last'), '.4f')}",
            f"- Options skew (per $1k) last: {fmt(market_summary.get('options_skew_per_1k_last'), '.6f')}",
            f"- Settlement gap (hours) lo/hi: {fmt(market_summary.get('settlement_gap_hours_lo'), '.2f')} / "
            f"{fmt(market_summary.get('settlement_gap_hours_hi'), '.2f')}",
            f"- Options basis EWMA last: {fmt(market_summary.get('basis_ewma_options_last'))}",
            f"- Options residual mean / p95-abs: {fmt(market_summary.get('residual_options_mean'))} / "
            f"{fmt(market_summary.get('residual_options_abs_p95'))}",
            f"- Level corr options-fair vs Poly: {fmt(market_summary.get('corr_level_options_fair_vs_poly_mid'), '.4f')}",
            "",
        ]
    lines += [
        "## Files",
        "",
        "- `samples.csv`: full time-series samples.",
        "- `final_summary.json`: final cumulative summary.",
        "- `stages/stage_summaries.jsonl`: one JSON object per stage.",
        "- `stages/stage_*.json`: detailed stage summaries every configured interval.",
        "- `market_*_fair_vs_poly.png`: three-layer fair / Poly / deviation plots.",
        "",
    ]
    (out_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")


def run_capture(args: argparse.Namespace) -> dict[str, Any]:
    out_dir = Path(args.out_dir or f"runs/btc_above_fair_research_{utc_stamp()}")
    out_dir.mkdir(parents=True, exist_ok=True)

    markets = discover_btc_above_markets(args)
    override_expiry_ts = parse_datetime_to_unix(args.expiry_utc) if args.expiry_utc else 0
    if override_expiry_ts:
        markets = [dataclasses.replace(market, end_ts=override_expiry_ts) for market in markets]
    all_asset_ids: list[str] = []
    for market in markets:
        all_asset_ids.extend([market.yes_token_id, market.no_token_id])

    oracle = OracleState(vol_window_seconds=args.realized_vol_window_seconds)
    poly = PolyState(markets)
    options: OptionsState | None = None
    if args.enable_options_fair:
        options = OptionsState(
            markets,
            currency=args.options_currency,
            strike_window=args.options_strike_window,
            refresh_interval_sec=args.options_refresh_seconds,
            delta_k=args.options_delta_k,
            max_strikes=args.options_max_strikes,
            instruments_ttl_sec=args.options_instruments_ttl_seconds,
        )
        try:
            options.refresh()
        except Exception as exc:  # noqa: BLE001
            options.errors.append(f"warm_refresh: {exc}")
    stop = threading.Event()
    ws_errors: list[str] = []

    threads = [
        threading.Thread(
            target=ws_loop,
            kwargs={
                "name": "binance_book_ticker",
                "url": f"{BINANCE_VISION_WS}/btcusdt@bookTicker",
                "stop": stop,
                "on_message": oracle.observe_book_ticker,
                "errors": ws_errors,
            },
            daemon=True,
        ),
        threading.Thread(
            target=ws_loop,
            kwargs={
                "name": "binance_depth5",
                "url": f"{BINANCE_VISION_WS}/btcusdt@depth5@100ms",
                "stop": stop,
                "on_message": oracle.observe_depth5,
                "errors": ws_errors,
            },
            daemon=True,
        ),
        threading.Thread(
            target=ws_loop,
            kwargs={
                "name": "polymarket_market",
                "url": POLYMARKET_WS,
                "stop": stop,
                "on_message": poly.observe_ws_payload,
                "on_open_send": [
                    {
                        "type": "market",
                        "assets_ids": all_asset_ids,
                        "custom_feature_enabled": True,
                    }
                ],
                "errors": ws_errors,
            },
            daemon=True,
        ),
    ]
    if options is not None:
        threads.append(
            threading.Thread(target=options.loop, kwargs={"stop": stop}, daemon=True)
        )
    for thread in threads:
        thread.start()

    start_wall = time.time()
    end_wall = start_wall + float(args.duration_seconds)
    metadata = {
        "script": "poly_btc_above_fair_research.py",
        "start_wall_unix_seconds": start_wall,
        "start_iso_utc": iso_utc_from_unix(start_wall),
        "duration_seconds": args.duration_seconds,
        "sample_interval_ms": args.sample_interval_ms,
        "stage_interval_seconds": args.stage_interval_seconds,
        "date_label": args.date_label,
        "expiry_utc": args.expiry_utc,
        "reference_exchange": args.reference_exchange,
        "reference_symbol": args.reference_symbol,
        "resolution_candle_interval": args.resolution_candle_interval,
        "resolution_field": args.resolution_field,
        "expected_poly_basis": args.expected_poly_basis,
        "fair_vol_mode": args.fair_vol_mode,
        "vol_annual_bps": args.vol_annual_bps,
        "drift_annual_bps": args.drift_annual_bps,
        "basis_tau_sec": args.basis_tau_sec,
        "max_healthy_spread": args.max_healthy_spread,
        "max_quote_age_ms": args.max_quote_age_ms,
        "options_fair_enabled": bool(args.enable_options_fair),
        "options_currency": args.options_currency,
        "options_strike_window": args.options_strike_window,
        "options_refresh_seconds": args.options_refresh_seconds,
        "options_delta_k": args.options_delta_k,
        "options_max_strikes": args.options_max_strikes,
        "options_surface": (
            options.snapshot(markets[0].market_id) if options is not None and markets else {}
        ),
        "markets": [dataclasses.asdict(m) for m in markets],
    }
    (out_dir / "metadata.initial.json").write_text(json.dumps(metadata, indent=2, allow_nan=True) + "\n", encoding="utf-8")

    basis_trackers = {
        market.market_id: {
            "fixed": BasisTracker(tau_sec=args.basis_tau_sec),
            "options": BasisTracker(tau_sec=args.basis_tau_sec),
        }
        for market in markets
    }
    fieldnames = make_sample_fieldnames(markets)
    samples_path = out_dir / "samples.csv"
    write_csv_header_if_needed(samples_path, fieldnames)

    rows: list[dict[str, Any]] = []
    stage_rows: list[dict[str, Any]] = []
    sample_interval_ns = int(args.sample_interval_ms * 1_000_000)
    next_stage_wall = start_wall + args.stage_interval_seconds
    stage_idx = 1
    sample_idx = 0

    try:
        while time.time() < end_wall and not stop.is_set():
            target_ns = int(start_wall * 1_000_000_000) + sample_idx * sample_interval_ns
            sleep_s = (target_ns - now_ns()) / 1_000_000_000.0
            if sleep_s > 0:
                time.sleep(sleep_s)
            ts_ns = now_ns()
            row = compute_row(
                idx=sample_idx,
                start_wall=start_wall,
                ts_ns=ts_ns,
                oracle=oracle,
                poly=poly,
                options=options,
                markets=markets,
                basis_trackers=basis_trackers,
                args=args,
            )
            append_csv_row(samples_path, fieldnames, row)
            rows.append(row)
            stage_rows.append(row)
            sample_idx += 1

            if time.time() >= next_stage_wall:
                write_stage_summary(
                    out_dir=out_dir,
                    stage_idx=stage_idx,
                    all_rows=rows,
                    stage_rows=stage_rows,
                    markets=markets,
                    metadata=metadata,
                )
                print(f"stage_written index={stage_idx} samples={len(stage_rows)} out_dir={out_dir}", flush=True)
                stage_idx += 1
                stage_rows = []
                next_stage_wall += args.stage_interval_seconds
    finally:
        stop.set()
        for thread in threads:
            thread.join(timeout=2.0)

    if stage_rows:
        write_stage_summary(
            out_dir=out_dir,
            stage_idx=stage_idx,
            all_rows=rows,
            stage_rows=stage_rows,
            markets=markets,
            metadata=metadata,
        )

    final_summary = summarize_rows(rows, markets)
    summary = {
        "metadata": metadata,
        "out_dir": str(out_dir),
        "final_summary": final_summary,
        "binance_book_ticker_events": oracle.book_ticker_events,
        "binance_depth_events": oracle.depth_events,
        "polymarket_ws_messages": poly.ws_messages,
        "polymarket_book_snapshots": poly.book_snapshots,
        "polymarket_price_changes": poly.price_changes,
        "options_refresh_count": options.refresh_count if options is not None else 0,
        "options_http_calls": options.http_calls if options is not None else 0,
        "options_errors": (options.errors[-50:] if options is not None else []),
        "ws_errors": ws_errors[-50:],
        "polymarket_errors": poly.errors[-50:],
    }
    (out_dir / "final_summary.json").write_text(json.dumps(summary, indent=2, allow_nan=True) + "\n", encoding="utf-8")
    if not args.no_plots:
        make_plots(out_dir, rows, markets)
    write_report(out_dir, summary)
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration-seconds", type=int, default=3600)
    parser.add_argument("--sample-interval-ms", type=float, default=250.0)
    parser.add_argument("--stage-interval-seconds", type=int, default=600, help="Write stage summary every N seconds. Default: 10 minutes.")
    parser.add_argument("--out-dir", default="")

    parser.add_argument("--date-label", default="June 6", help="Question substring filter, e.g. 'June 6'.")
    parser.add_argument("--market-slugs", default="", help="Comma-separated exact Gamma slugs. Overrides discovery.")
    parser.add_argument("--thresholds", default="62000", help="Comma-separated thresholds to keep, e.g. 62000.")
    parser.add_argument("--max-markets", type=int, default=1)
    parser.add_argument("--gamma-scan-limit", type=int, default=3000)
    parser.add_argument("--include-closed", action="store_true")
    parser.add_argument("--window-end-unix-seconds", type=int, default=1780761600, help="Fallback expiry if Gamma row lacks endDate. Default: 2026-06-06 16:00:00 UTC.")
    parser.add_argument("--expiry-utc", default="2026-06-06T16:00:00+00:00", help="Override market settlement timestamp for fair-value T. Default matches Jun 6 12:00 PM ET / 16:00 UTC.")
    parser.add_argument("--reference-exchange", default="Binance")
    parser.add_argument("--reference-symbol", default="BTCUSDT")
    parser.add_argument("--resolution-candle-interval", default="1m")
    parser.add_argument("--resolution-field", default="close")
    parser.add_argument("--expected-poly-basis", type=float, default=0.0117, help="Research-only expected Poly basis added to options_fair. No trade signals are produced.")
    parser.add_argument("--reference-spot", type=float, default=math.nan, help="Used to select closest thresholds when auto-discovering.")

    parser.add_argument("--fair-vol-mode", choices=["fixed", "realized"], default="fixed")
    parser.add_argument("--vol-annual-bps", type=float, default=1800.0)
    parser.add_argument("--drift-annual-bps", type=float, default=0.0)
    parser.add_argument("--realized-vol-window-seconds", type=float, default=300.0)

    parser.add_argument("--basis-tau-sec", type=float, default=1800.0, help="EWMA time constant for structural basis.")
    parser.add_argument("--max-healthy-spread", type=float, default=0.05, help="Probability units. 0.05 = 5 cents.")
    parser.add_argument("--max-quote-age-ms", type=float, default=5000.0)

    parser.add_argument(
        "--enable-options-fair",
        dest="enable_options_fair",
        action="store_true",
        default=True,
        help="Fair C: Deribit options-implied digital (primary benchmark). On by default.",
    )
    parser.add_argument(
        "--no-options-fair",
        dest="enable_options_fair",
        action="store_false",
        help="Disable the options-implied digital fair (skip Deribit).",
    )
    parser.add_argument("--options-currency", default="BTC")
    parser.add_argument("--options-strike-window", type=float, default=6000.0, help="USD half-width of strikes around K used to build the smile.")
    parser.add_argument("--options-refresh-seconds", type=float, default=15.0, help="How often to refetch the Deribit surface.")
    parser.add_argument("--options-delta-k", type=float, default=500.0, help="Call-spread half-width dK (USD) for the digital extraction.")
    parser.add_argument("--options-max-strikes", type=int, default=9, help="Max strikes per expiry to fetch around K.")
    parser.add_argument("--options-instruments-ttl-seconds", type=float, default=600.0, help="Cache TTL for the Deribit instrument list.")

    parser.add_argument("--no-plots", action="store_true")
    return parser.parse_args()


def main() -> int:
    summary = run_capture(parse_args())
    print("btc_above_fair_research:")
    print(f"  out_dir: {summary['out_dir']}")
    print(f"  samples: {summary['final_summary']['sample_count']}")
    print(f"  options_refresh_count: {summary.get('options_refresh_count', 0)}")

    def show(value: Any, spec: str = ".6f") -> str:
        try:
            number_value = float(value)
        except (TypeError, ValueError):
            return "nan"
        return f"{number_value:{spec}}" if math.isfinite(number_value) else "nan"

    for item in summary["final_summary"].get("markets", []):
        print(f"  market: {item['question']}")
        print(f"    threshold: {item['threshold']}")
        print(f"    options_samples: {item.get('options_sample_count', 0)}")
        print(f"    D_fixed_mean:    {show(item.get('deviation_fixed_mean'))}")
        print(f"    D_realized_mean: {show(item.get('deviation_realized_mean'))}")
        print(f"    D_options_mean:  {show(item.get('deviation_options_mean'))}  <- primary")
        print(f"    expected_poly_fair_last: {show(item.get('expected_poly_fair_yes_last'))}")
        print(f"    residual_vs_expected_poly_fair_mean: {show(item.get('deviation_vs_expected_poly_fair_mean'))}")
        print(f"    options_fair_last: {show(item.get('options_fair_last'))}")
        print(f"    options_strike_iv_last: {show(item.get('options_strike_iv_last'), '.4f')}")
        print(
            "    settlement_gap_hours lo/hi: "
            f"{show(item.get('settlement_gap_hours_lo'), '.2f')} / {show(item.get('settlement_gap_hours_hi'), '.2f')}"
        )
    print(f"  report: {Path(summary['out_dir']) / 'report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

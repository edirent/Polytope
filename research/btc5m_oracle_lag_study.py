#!/usr/bin/env python3
"""Read-only BTC 5m oracle lag study.

Captures one Polymarket BTC Up/Down 5m window at a fixed sampling interval and
joins it with Binance Vision BTCUSDT spot data. It does not place orders or use
private credentials.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import dataclasses
import datetime as dt
import json
import math
import os
from pathlib import Path
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


BINANCE_VISION_WS = "wss://data-stream.binance.vision/ws"
POLYMARKET_WS = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
GAMMA_MARKETS = "https://gamma-api.polymarket.com/markets"
POLYMARKET_TRADES = "https://data-api.polymarket.com/trades"
SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0


def now_ns() -> int:
    return time.time_ns()


def utc_stamp() -> str:
    return time.strftime("%Y%m%d_%H%M%S", time.gmtime())


def http_json(url: str, timeout_s: int = 15) -> Any:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "PolytopeBTC5mLagStudy/0.1",
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


def digital_call_probability(
    *,
    spot: float,
    strike: float,
    seconds_to_expiry: float,
    vol_annual_bps: float,
    drift_annual_bps: float,
) -> float:
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


def percentile(values: list[float], q: float) -> float:
    clean = sorted(v for v in values if math.isfinite(v))
    if not clean:
        return math.nan
    idx = min(len(clean) - 1, max(0, int(round((len(clean) - 1) * q))))
    return clean[idx]


@dataclasses.dataclass
class MarketInfo:
    slug: str
    question: str
    condition_id: str
    market_id: str
    start_ts: int
    end_ts: int
    up_token_id: str
    down_token_id: str


def discover_btc5m_market(start_ts: int) -> MarketInfo:
    slug = f"btc-updown-5m-{start_ts}"
    query = urllib.parse.urlencode({"slug": slug})
    data = http_json(f"{GAMMA_MARKETS}?{query}")
    if not isinstance(data, list) or not data:
        raise RuntimeError(f"Polymarket BTC 5m market not found for slug={slug}")
    market = data[0]
    outcomes = [str(x) for x in parse_json_array(market.get("outcomes"))]
    tokens = [str(x) for x in parse_json_array(market.get("clobTokenIds"))]
    if len(tokens) < 2 or [x.lower() for x in outcomes[:2]] != ["up", "down"]:
        raise RuntimeError(f"unexpected market tokens/outcomes for {slug}: {outcomes} {tokens}")
    return MarketInfo(
        slug=slug,
        question=str(market.get("question") or slug),
        condition_id=str(market.get("conditionId") or ""),
        market_id=str(market.get("conditionId") or market.get("id") or slug),
        start_ts=start_ts,
        end_ts=start_ts + 300,
        up_token_id=tokens[0],
        down_token_id=tokens[1],
    )


class OracleState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
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
            cutoff = ts - 10_000_000_000
            while self.mid_events and self.mid_events[0][0] < cutoff:
                self.mid_events.pop(0)
            while self.ofi_events and self.ofi_events[0][0] < cutoff:
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
                "ofi_100ms": ofi_100,
                "ofi_500ms": ofi_500,
                "spot_depth5_bid_qty": self.depth5_bid_qty,
                "spot_depth5_ask_qty": self.depth5_ask_qty,
                "spot_depth5_imbalance": self.depth5_imbalance,
            }


@dataclasses.dataclass
class PolyBook:
    bids: dict[float, float] = dataclasses.field(default_factory=dict)
    asks: dict[float, float] = dataclasses.field(default_factory=dict)
    best_bid: float = math.nan
    best_ask: float = math.nan
    last_trade_price: float = math.nan

    def update_best(self) -> None:
        self.best_bid = max((p for p, s in self.bids.items() if s > 0), default=math.nan)
        self.best_ask = min((p for p, s in self.asks.items() if s > 0), default=math.nan)

    def depth5(self) -> tuple[float, float]:
        top_bids = sorted(((p, s) for p, s in self.bids.items() if s > 0), reverse=True)[:5]
        top_asks = sorted((p, s) for p, s in self.asks.items() if s > 0)[:5]
        return sum(s for _, s in top_bids), sum(s for _, s in top_asks)


class PolyState:
    def __init__(self, market: MarketInfo) -> None:
        self.market = market
        self.lock = threading.Lock()
        self.books = {
            market.up_token_id: PolyBook(),
            market.down_token_id: PolyBook(),
        }
        self.book_snapshots = 0
        self.price_changes = 0
        self.ws_messages = 0
        self.errors: list[str] = []
        self.fills: list[dict[str, Any]] = []
        self.seen_fill_keys: set[str] = set()

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
            price = number(level.get("price") if isinstance(level, dict) else None)
            size = number(level.get("size") if isinstance(level, dict) else None, 0.0)
            if math.isfinite(price) and price > 0 and size > 0:
                book.bids[price] = size
        for level in payload.get("asks", []) or []:
            price = number(level.get("price") if isinstance(level, dict) else None)
            size = number(level.get("size") if isinstance(level, dict) else None, 0.0)
            if math.isfinite(price) and price > 0 and size > 0:
                book.asks[price] = size
        book.last_trade_price = number(payload.get("last_trade_price"))
        book.update_best()
        with self.lock:
            self.books[asset] = book
            self.book_snapshots += 1

    def _observe_price_change(self, payload: dict[str, Any]) -> None:
        changes = payload.get("price_changes")
        if not isinstance(changes, list):
            return
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
            self.price_changes += len(changes)

    def add_fills(self, fills: list[dict[str, Any]]) -> None:
        with self.lock:
            for fill in fills:
                key = str(fill.get("transactionHash") or "") + ":" + str(fill.get("asset") or "") + ":" + str(fill.get("side") or "") + ":" + str(fill.get("timestamp") or "") + ":" + str(fill.get("price") or "") + ":" + str(fill.get("size") or "")
                if not key or key in self.seen_fill_keys:
                    continue
                self.seen_fill_keys.add(key)
                self.fills.append(fill)

    def snapshot(self) -> dict[str, float]:
        with self.lock:
            book = self.books[self.market.up_token_id]
            bid = book.best_bid
            ask = book.best_ask
            mid = (bid + ask) / 2.0 if math.isfinite(bid) and math.isfinite(ask) and bid > 0 and ask > 0 else math.nan
            bid_depth5, ask_depth5 = book.depth5()
            return {
                "poly_bid": bid,
                "poly_ask": ask,
                "poly_mid": mid,
                "poly_spread": ask - bid if math.isfinite(mid) else math.nan,
                "poly_depth5_bid_qty": bid_depth5,
                "poly_depth5_ask_qty": ask_depth5,
                "poly_depth5_total_qty": bid_depth5 + ask_depth5,
                "poly_last_trade_price": book.last_trade_price,
                "fill_total": float(len(self.fills)),
            }

    def fills_copy(self) -> list[dict[str, Any]]:
        with self.lock:
            return list(self.fills)


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


def trade_poll_loop(
    *,
    market: MarketInfo,
    state: PolyState,
    start_ts: int,
    end_ts: int,
    poll_interval_s: float,
    trade_limit: int,
    stop: threading.Event,
) -> None:
    query = urllib.parse.urlencode({"market": market.condition_id, "limit": trade_limit})
    url = f"{POLYMARKET_TRADES}?{query}"
    while not stop.is_set():
        try:
            data = http_json(url, timeout_s=10)
            if isinstance(data, list):
                filtered = []
                for row in data:
                    if not isinstance(row, dict):
                        continue
                    ts = int(number(row.get("timestamp"), 0.0))
                    if start_ts <= ts <= end_ts + 2:
                        filtered.append(row)
                state.add_fills(filtered)
        except Exception as exc:  # noqa: BLE001
            with state.lock:
                state.errors.append(f"trade_poll: {exc}")
        stop.wait(poll_interval_s)


def fill_velocity_columns(rows: list[dict[str, Any]]) -> None:
    times = [int(row["ts_ns"]) for row in rows]
    mids = [float(row["spot_mid"]) for row in rows]
    for row in rows:
        ts = int(row["ts_ns"])
        idx_100 = bisect.bisect_right(times, ts - 100_000_000) - 1
        idx_500 = bisect.bisect_right(times, ts - 500_000_000) - 1
        spot = float(row["spot_mid"])
        for key, idx in (("v_100ms_bps", idx_100), ("v_500ms_bps", idx_500)):
            if idx >= 0 and math.isfinite(spot) and math.isfinite(mids[idx]) and mids[idx] > 0:
                row[key] = (spot / mids[idx] - 1.0) * 10_000.0
            else:
                row[key] = math.nan


def compute_lag_metrics(
    rows: list[dict[str, Any]],
    *,
    jump_threshold_bps: float,
    poly_follow_threshold: float,
    max_lag_ms: int,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    events: list[dict[str, Any]] = []
    refractory_ns = 500_000_000
    last_event_ns = -10**30
    times = [int(row["ts_ns"]) for row in rows]
    for idx, row in enumerate(rows):
        ts = int(row["ts_ns"])
        if ts - last_event_ns < refractory_ns:
            continue
        v = float(row.get("v_100ms_bps", math.nan))
        base_mid = float(row.get("poly_mid", math.nan))
        if not math.isfinite(v) or abs(v) < jump_threshold_bps or not math.isfinite(base_mid):
            continue
        sign = 1.0 if v > 0 else -1.0
        deadline = ts + max_lag_ms * 1_000_000
        lag_ms = math.nan
        for j in range(idx + 1, len(rows)):
            if times[j] > deadline:
                break
            mid = float(rows[j].get("poly_mid", math.nan))
            if math.isfinite(mid) and sign * (mid - base_mid) >= poly_follow_threshold:
                lag_ms = (times[j] - ts) / 1_000_000.0
                break
        events.append(
            {
                "ts_ns": ts,
                "rel_ms": row["rel_ms"],
                "spot_jump_100ms_bps": v,
                "poly_mid_at_jump": base_mid,
                "lag_ms": lag_ms,
            }
        )
        last_event_ns = ts

    fair = np.array([float(row.get("fair", math.nan)) for row in rows])
    mid = np.array([float(row.get("poly_mid", math.nan)) for row in rows])
    fair_ret = np.diff(fair)
    mid_ret = np.diff(mid)
    interval_ms = statistics.median(
        [
            (int(rows[i]["ts_ns"]) - int(rows[i - 1]["ts_ns"])) / 1_000_000.0
            for i in range(1, min(len(rows), 100))
        ]
    ) if len(rows) > 2 else 50.0
    max_steps = max(1, int(max_lag_ms / max(interval_ms, 1.0)))
    xcorr: list[dict[str, float]] = []
    for lag in range(max_steps + 1):
        if lag == 0:
            x = fair_ret
            y = mid_ret
        else:
            x = fair_ret[:-lag]
            y = mid_ret[lag:]
        mask = np.isfinite(x) & np.isfinite(y)
        corr = math.nan
        if int(mask.sum()) >= 10 and np.std(x[mask]) > 0 and np.std(y[mask]) > 0:
            corr = float(np.corrcoef(x[mask], y[mask])[0, 1])
        xcorr.append({"lag_ms": lag * interval_ms, "corr": corr})
    valid_corr = [row for row in xcorr if math.isfinite(row["corr"])]
    best = max(valid_corr, key=lambda row: row["corr"], default={"lag_ms": math.nan, "corr": math.nan})
    summary = {
        "jump_event_count": len(events),
        "jump_lag_count": sum(1 for event in events if math.isfinite(float(event["lag_ms"]))),
        "jump_lag_mean_ms": statistics.mean([float(event["lag_ms"]) for event in events if math.isfinite(float(event["lag_ms"]))]) if any(math.isfinite(float(event["lag_ms"])) for event in events) else math.nan,
        "xcorr_best_lag_ms": best["lag_ms"],
        "xcorr_best_corr": best["corr"],
        "xcorr": xcorr,
    }
    return events, summary


def normalized_up_fill(market: MarketInfo, fill: dict[str, Any]) -> dict[str, Any] | None:
    asset = str(fill.get("asset") or "")
    side = str(fill.get("side") or "").upper()
    price = number(fill.get("price"))
    size = number(fill.get("size"), 0.0)
    ts = int(number(fill.get("timestamp"), 0.0))
    if asset not in {market.up_token_id, market.down_token_id} or side not in {"BUY", "SELL"}:
        return None
    if not math.isfinite(price) or price <= 0 or size <= 0 or ts <= 0:
        return None
    if asset == market.up_token_id:
        up_price = price
        direction = 1 if side == "BUY" else -1
    else:
        up_price = 1.0 - price
        direction = -1 if side == "BUY" else 1
    return {
        "timestamp": ts,
        "ts_ns": ts * 1_000_000_000,
        "asset": asset,
        "raw_side": side,
        "raw_price": price,
        "up_price": up_price,
        "size": size,
        "up_taker_direction": direction,
        "outcome": fill.get("outcome", ""),
        "transactionHash": fill.get("transactionHash", ""),
    }


def compute_markouts(
    market: MarketInfo,
    rows: list[dict[str, Any]],
    fills: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    times = [int(row["ts_ns"]) for row in rows]
    out: list[dict[str, Any]] = []
    for fill in fills:
        item = normalized_up_fill(market, fill)
        if not item:
            continue
        idx = bisect.bisect_left(times, int(item["ts_ns"]))
        if idx >= len(rows):
            continue
        base_mid = float(rows[idx].get("poly_mid", math.nan))
        if not math.isfinite(base_mid):
            continue
        item["rel_ms"] = rows[idx]["rel_ms"]
        item["poly_mid_at_fill"] = base_mid
        for horizon_s in (1, 5):
            j = bisect.bisect_left(times, int(item["ts_ns"]) + horizon_s * 1_000_000_000)
            future_mid = float(rows[j].get("poly_mid", math.nan)) if j < len(rows) else math.nan
            raw = future_mid - base_mid if math.isfinite(future_mid) else math.nan
            item[f"poly_mid_{horizon_s}s"] = future_mid
            item[f"markout_{horizon_s}s"] = raw
            item[f"directional_markout_{horizon_s}s"] = raw * item["up_taker_direction"] if math.isfinite(raw) else math.nan
        out.append(item)
    return out


def write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str] | None = None) -> None:
    if fieldnames is None:
        fieldnames = list(rows[0].keys()) if rows else []
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def make_plots(out_dir: Path, rows: list[dict[str, Any]], lag_summary: dict[str, Any], markouts: list[dict[str, Any]]) -> None:
    rel_s = np.array([float(row["rel_ms"]) / 1000.0 for row in rows])
    fair = np.array([float(row.get("fair", math.nan)) for row in rows])
    poly_mid = np.array([float(row.get("poly_mid", math.nan)) for row in rows])
    deviation = poly_mid - fair

    fig, axes = plt.subplots(2, 1, figsize=(12, 8), sharex=True)
    axes[0].plot(rel_s, fair, label="Fair", linewidth=1.2)
    axes[0].plot(rel_s, poly_mid, label="Polymarket Mid", linewidth=1.0)
    axes[0].set_ylabel("Probability")
    axes[0].legend(loc="best")
    axes[0].grid(True, alpha=0.25)
    axes[1].plot(rel_s, deviation, label="D = Mid - Fair", color="tab:red", linewidth=1.0)
    axes[1].axhline(0, color="black", linewidth=0.8)
    axes[1].set_ylabel("Deviation")
    axes[1].set_xlabel("Seconds From Window Start")
    axes[1].grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_dir / "deviation.png", dpi=140)
    plt.close(fig)

    xcorr = lag_summary.get("xcorr", [])
    fig, ax = plt.subplots(figsize=(12, 4))
    ax.plot(
        [float(row["lag_ms"]) for row in xcorr],
        [float(row["corr"]) if math.isfinite(float(row["corr"])) else np.nan for row in xcorr],
        linewidth=1.2,
    )
    ax.axvline(float(lag_summary.get("xcorr_best_lag_ms", math.nan)), color="tab:red", linestyle="--", linewidth=1.0)
    ax.set_title("Fair Return -> Polymarket Mid Return Cross-Correlation")
    ax.set_xlabel("Lag ms")
    ax.set_ylabel("Correlation")
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_dir / "oracle_lag.png", dpi=140)
    plt.close(fig)

    fig, axes = plt.subplots(2, 1, figsize=(12, 7))
    xs = [float(row["rel_ms"]) / 1000.0 for row in markouts if math.isfinite(float(row.get("directional_markout_1s", math.nan)))]
    ys = [float(row["directional_markout_1s"]) for row in markouts if math.isfinite(float(row.get("directional_markout_1s", math.nan)))]
    axes[0].scatter(xs, ys, s=18, alpha=0.75)
    axes[0].axhline(0, color="black", linewidth=0.8)
    axes[0].set_ylabel("Directional 1s Markout")
    axes[0].grid(True, alpha=0.25)
    axes[1].hist(ys, bins=30, alpha=0.75)
    axes[1].axvline(0, color="black", linewidth=0.8)
    axes[1].set_xlabel("Directional 1s Markout")
    axes[1].set_ylabel("Fill Count")
    axes[1].grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_dir / "markout.png", dpi=140)
    plt.close(fig)


def run_capture(args: argparse.Namespace) -> dict[str, Any]:
    out_dir = Path(args.out_dir or f"runs/btc5m_oracle_lag_{utc_stamp()}")
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.start_ts > 0:
        start_ts = int(args.start_ts)
        start_wall = float(start_ts)
    elif args.start_now:
        start_wall = time.time()
        start_ts = int(start_wall // 300 * 300)
    else:
        now = time.time()
        start_ts = int(math.ceil((now + args.min_wait_seconds) / 300.0) * 300)
        start_wall = float(start_ts)

    market = discover_btc5m_market(start_ts)
    end_wall = start_wall + float(args.duration_seconds)
    oracle = OracleState()
    poly = PolyState(market)
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
                        "assets_ids": [market.up_token_id, market.down_token_id],
                        "custom_feature_enabled": True,
                    }
                ],
                "errors": ws_errors,
            },
            daemon=True,
        ),
        threading.Thread(
            target=trade_poll_loop,
            kwargs={
                "market": market,
                "state": poly,
                "start_ts": int(start_wall),
                "end_ts": int(end_wall),
                "poll_interval_s": args.trade_poll_interval_s,
                "trade_limit": args.trade_limit,
                "stop": stop,
            },
            daemon=True,
        ),
    ]
    for thread in threads:
        thread.start()

    metadata = {
        "market": dataclasses.asdict(market),
        "oracle_source": "Binance Vision BTCUSDT spot bookTicker + depth5@100ms",
        "sample_interval_ms": args.sample_interval_ms,
        "duration_seconds": args.duration_seconds,
        "start_wall_unix_seconds": start_wall,
        "end_wall_unix_seconds": end_wall,
        "vol_annual_bps": args.vol_annual_bps,
        "drift_annual_bps": args.drift_annual_bps,
        "jump_threshold_bps": args.jump_threshold_bps,
        "poly_follow_threshold": args.poly_follow_threshold,
    }
    (out_dir / "metadata.initial.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    pre_wait = start_wall - time.time()
    if pre_wait > 0:
        print(f"waiting_for_window_start_seconds={pre_wait:.3f} slug={market.slug}", flush=True)
        while time.time() < start_wall and not stop.is_set():
            time.sleep(min(1.0, max(0.0, start_wall - time.time())))

    sample_interval_ns = int(args.sample_interval_ms * 1_000_000)
    sample_count = int(math.ceil(args.duration_seconds * 1000.0 / args.sample_interval_ms))
    rows: list[dict[str, Any]] = []
    strike = math.nan
    previous_fill_total = 0.0
    for idx in range(sample_count):
        target_ns = int(start_wall * 1_000_000_000) + idx * sample_interval_ns
        sleep_s = (target_ns - now_ns()) / 1_000_000_000.0
        if sleep_s > 0:
            time.sleep(sleep_s)
        ts = now_ns()
        oracle_snapshot = oracle.snapshot(ts)
        poly_snapshot = poly.snapshot()
        spot_mid = float(oracle_snapshot["spot_mid"])
        if not math.isfinite(strike) and math.isfinite(spot_mid) and spot_mid > 0:
            strike = spot_mid
        seconds_to_expiry = max(0.0, end_wall - ts / 1_000_000_000.0)
        fair = digital_call_probability(
            spot=spot_mid,
            strike=strike,
            seconds_to_expiry=seconds_to_expiry,
            vol_annual_bps=args.vol_annual_bps,
            drift_annual_bps=args.drift_annual_bps,
        )
        fill_total = float(poly_snapshot["fill_total"])
        row = {
            "sample_index": idx,
            "ts_ns": ts,
            "unix_seconds": ts / 1_000_000_000.0,
            "rel_ms": (ts / 1_000_000_000.0 - start_wall) * 1000.0,
            "seconds_to_expiry": seconds_to_expiry,
            "strike": strike,
            **oracle_snapshot,
            "v_100ms_bps": math.nan,
            "v_500ms_bps": math.nan,
            "fair": fair,
            **poly_snapshot,
            "fill_count_since_prev": fill_total - previous_fill_total,
        }
        previous_fill_total = fill_total
        rows.append(row)

    stop.set()
    for thread in threads:
        thread.join(timeout=2.0)

    fill_velocity_columns(rows)
    lag_events, lag_summary = compute_lag_metrics(
        rows,
        jump_threshold_bps=args.jump_threshold_bps,
        poly_follow_threshold=args.poly_follow_threshold,
        max_lag_ms=args.max_lag_ms,
    )
    fills = poly.fills_copy()
    markouts = compute_markouts(market, rows, fills)

    sample_fields = [
        "sample_index",
        "ts_ns",
        "unix_seconds",
        "rel_ms",
        "seconds_to_expiry",
        "strike",
        "spot_bid",
        "spot_ask",
        "spot_bid_qty",
        "spot_ask_qty",
        "spot_mid",
        "v_100ms_bps",
        "v_500ms_bps",
        "ofi_100ms",
        "ofi_500ms",
        "spot_depth5_bid_qty",
        "spot_depth5_ask_qty",
        "spot_depth5_imbalance",
        "fair",
        "poly_bid",
        "poly_ask",
        "poly_mid",
        "poly_spread",
        "poly_depth5_bid_qty",
        "poly_depth5_ask_qty",
        "poly_depth5_total_qty",
        "poly_last_trade_price",
        "fill_total",
        "fill_count_since_prev",
    ]
    write_csv(out_dir / "samples_50ms.csv", rows, sample_fields)
    write_csv(out_dir / "lag_events.csv", lag_events)
    write_csv(out_dir / "fills_markout.csv", markouts)
    (out_dir / "fills_raw.json").write_text(json.dumps(fills, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    deviations = [
        float(row["poly_mid"]) - float(row["fair"])
        for row in rows
        if math.isfinite(float(row.get("poly_mid", math.nan))) and math.isfinite(float(row.get("fair", math.nan)))
    ]
    directional_1s = [float(row["directional_markout_1s"]) for row in markouts if math.isfinite(float(row.get("directional_markout_1s", math.nan)))]
    directional_5s = [float(row["directional_markout_5s"]) for row in markouts if math.isfinite(float(row.get("directional_markout_5s", math.nan)))]
    summary = {
        **metadata,
        "out_dir": str(out_dir),
        "sample_count": len(rows),
        "actual_duration_seconds": (int(rows[-1]["ts_ns"]) - int(rows[0]["ts_ns"])) / 1_000_000_000.0 if len(rows) >= 2 else 0.0,
        "strike": strike,
        "binance_book_ticker_events": oracle.book_ticker_events,
        "binance_depth_events": oracle.depth_events,
        "polymarket_ws_messages": poly.ws_messages,
        "polymarket_book_snapshots": poly.book_snapshots,
        "polymarket_price_changes": poly.price_changes,
        "polymarket_fill_count": len(fills),
        "markout_fill_count": len(markouts),
        "deviation_mean": statistics.mean(deviations) if deviations else math.nan,
        "deviation_p05": percentile(deviations, 0.05),
        "deviation_p50": percentile(deviations, 0.50),
        "deviation_p95": percentile(deviations, 0.95),
        "deviation_abs_p95": percentile([abs(x) for x in deviations], 0.95),
        "directional_markout_1s_mean": statistics.mean(directional_1s) if directional_1s else math.nan,
        "directional_markout_5s_mean": statistics.mean(directional_5s) if directional_5s else math.nan,
        "lag": {k: v for k, v in lag_summary.items() if k != "xcorr"},
        "ws_errors": ws_errors[-20:],
        "polymarket_errors": poly.errors[-20:],
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=True) + "\n", encoding="utf-8")
    if not args.no_plots:
        make_plots(out_dir, rows, lag_summary, markouts)
    write_report(out_dir, summary)
    return summary


def write_report(out_dir: Path, summary: dict[str, Any]) -> None:
    lag = summary.get("lag", {})
    lines = [
        "# BTC 5m Oracle Lag Study",
        "",
        "Read-only capture. No trading endpoints or private credentials were used.",
        "",
        f"- Market: {summary['market']['question']}",
        f"- Slug: `{summary['market']['slug']}`",
        f"- Oracle: {summary['oracle_source']}",
        f"- Samples: {summary['sample_count']} at {summary['sample_interval_ms']} ms",
        f"- Actual duration: {summary['actual_duration_seconds']:.3f} s",
        f"- Strike used for Fair: {summary['strike']:.2f}",
        f"- Vol assumption: {summary['vol_annual_bps']:.0f} bps annualized",
        "",
        "## Capture Counts",
        "",
        f"- Binance bookTicker events: {summary['binance_book_ticker_events']}",
        f"- Binance depth5 events: {summary['binance_depth_events']}",
        f"- Polymarket WS messages: {summary['polymarket_ws_messages']}",
        f"- Polymarket book snapshots: {summary['polymarket_book_snapshots']}",
        f"- Polymarket price changes: {summary['polymarket_price_changes']}",
        f"- Polymarket public trades captured: {summary['polymarket_fill_count']}",
        "",
        "## Absolute Deviation",
        "",
        f"- mean D: {summary['deviation_mean']:.6f}",
        f"- p05/p50/p95 D: {summary['deviation_p05']:.6f} / {summary['deviation_p50']:.6f} / {summary['deviation_p95']:.6f}",
        f"- p95 |D|: {summary['deviation_abs_p95']:.6f}",
        "",
        "## Oracle Lag",
        "",
        f"- spot jump threshold: {summary['jump_threshold_bps']} bps over 100ms",
        f"- detected jumps: {lag.get('jump_event_count', 0)}",
        f"- jumps with Polymarket follow-through: {lag.get('jump_lag_count', 0)}",
        f"- mean jump lag: {lag.get('jump_lag_mean_ms', math.nan):.3f} ms",
        f"- best cross-correlation lag: {lag.get('xcorr_best_lag_ms', math.nan):.3f} ms, corr={lag.get('xcorr_best_corr', math.nan):.4f}",
        "",
        "## Fill-Conditional Markout",
        "",
        f"- fills with markout rows: {summary['markout_fill_count']}",
        f"- mean directional 1s markout: {summary['directional_markout_1s_mean']:.6f}",
        f"- mean directional 5s markout: {summary['directional_markout_5s_mean']:.6f}",
        "",
        "## Files",
        "",
        "- `samples_50ms.csv`",
        "- `fills_raw.json`",
        "- `fills_markout.csv`",
        "- `lag_events.csv`",
        "- `deviation.png`",
        "- `oracle_lag.png`",
        "- `markout.png`",
        "- `summary.json`",
        "",
    ]
    if summary.get("ws_errors") or summary.get("polymarket_errors"):
        lines += ["## Errors", ""]
        for item in summary.get("ws_errors", []):
            lines.append(f"- `{item}`")
        for item in summary.get("polymarket_errors", []):
            lines.append(f"- `{item}`")
        lines.append("")
    (out_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration-seconds", type=int, default=300)
    parser.add_argument("--sample-interval-ms", type=float, default=50.0)
    parser.add_argument("--start-ts", type=int, default=0, help="5m market start unix seconds")
    parser.add_argument("--start-now", action="store_true", help="start immediately using current 5m market")
    parser.add_argument("--min-wait-seconds", type=float, default=15.0)
    parser.add_argument("--vol-annual-bps", type=float, default=8_000.0)
    parser.add_argument("--drift-annual-bps", type=float, default=0.0)
    parser.add_argument("--jump-threshold-bps", type=float, default=10.0)
    parser.add_argument("--poly-follow-threshold", type=float, default=0.005)
    parser.add_argument("--max-lag-ms", type=int, default=2_000)
    parser.add_argument("--trade-poll-interval-s", type=float, default=0.5)
    parser.add_argument("--trade-limit", type=int, default=1000)
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--no-plots", action="store_true")
    return parser.parse_args()


def main() -> int:
    summary = run_capture(parse_args())
    print("btc5m_oracle_lag_study:")
    print(f"  out_dir: {summary['out_dir']}")
    print(f"  market: {summary['market']['question']}")
    print(f"  samples: {summary['sample_count']}")
    print(f"  binance_book_ticker_events: {summary['binance_book_ticker_events']}")
    print(f"  binance_depth_events: {summary['binance_depth_events']}")
    print(f"  polymarket_price_changes: {summary['polymarket_price_changes']}")
    print(f"  polymarket_fill_count: {summary['polymarket_fill_count']}")
    print(f"  deviation_abs_p95: {summary['deviation_abs_p95']:.6f}")
    print(f"  xcorr_best_lag_ms: {summary['lag']['xcorr_best_lag_ms']:.3f}")
    print(f"  report: {Path(summary['out_dir']) / 'report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

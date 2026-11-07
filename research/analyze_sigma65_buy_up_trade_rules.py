#!/usr/bin/env python3
"""Trade-level replay for sigma65 BUY UP V1-V4 entry rules.

Each market can open at most one BUY UP trade. The entry is the first row that
matches the rule for the requested size. Position is held to settlement.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any


SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0
TAU_MIN = 60.0
TAU_MAX = 180.0
EDGE_THRESHOLD = 0.03


def finite(value: float) -> bool:
    return math.isfinite(value)


def number(value: Any, default: float = math.nan) -> float:
    if value is None or value == "":
        return default
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    return out if finite(out) else default


def mean(values: list[float]) -> float:
    clean = [v for v in values if finite(v)]
    return sum(clean) / len(clean) if clean else math.nan


def percentile(values: list[float], q: float) -> float:
    clean = sorted(v for v in values if finite(v))
    if not clean:
        return math.nan
    idx = min(len(clean) - 1, max(0, round((len(clean) - 1) * q)))
    return clean[idx]


def sanitize(value: Any) -> Any:
    if isinstance(value, float):
        return value if finite(value) else None
    if isinstance(value, dict):
        return {str(k): sanitize(v) for k, v in value.items()}
    if isinstance(value, list):
        return [sanitize(v) for v in value]
    return value


def vwap_buy(asks: list[list[float]], qty: float) -> float:
    remaining = qty
    notional = 0.0
    for level in asks:
        if len(level) < 2:
            continue
        price = number(level[0])
        size = number(level[1])
        if not (finite(price) and finite(size) and price > 0 and size > 0):
            continue
        take = min(remaining, size)
        notional += take * price
        remaining -= take
        if remaining <= 1e-12:
            return notional / qty
    return math.nan


def load_rows(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="", encoding="utf-8") as fh:
        rows = [dict(row) for row in csv.DictReader(fh)]
    rows.sort(key=lambda row: number(row["recv_ts"]))
    return rows


def load_q100_vwap(path: Path) -> dict[int, float]:
    out: dict[int, float] = {}
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            obj = json.loads(line)
            out[int(obj["sample_index"])] = vwap_buy(obj.get("up_asks") or [], 100.0)
    return out


def attach_q100(rows: list[dict[str, Any]], q100_vwap: dict[int, float]) -> None:
    for row in rows:
        idx = int(number(row["sample_index"]))
        vwap = q100_vwap.get(idx, math.nan)
        fair = number(row.get("fair_up_sigma_floor"))
        settlement = number(row.get("settlement_up"))
        row["vwap_buy_up_q100"] = vwap
        row["edge_buy_up_sigma_floor_q100"] = (
            fair - vwap if finite(fair) and finite(vwap) else math.nan
        )
        row["buy_up_settlement_pnl_q100"] = (
            settlement - vwap if finite(settlement) and finite(vwap) else math.nan
        )


def vwap_field(qty_label: str) -> str:
    return f"vwap_buy_up_{qty_label}"


def edge_field(qty_label: str) -> str:
    return f"edge_buy_up_sigma_floor_{qty_label}"


def base_signal(row: dict[str, Any], qty_label: str) -> bool:
    tau = number(row.get("tau_sec"))
    edge = number(row.get(edge_field(qty_label)))
    vwap = number(row.get(vwap_field(qty_label)))
    fair_delta = number(row.get("fair_sigma_floor_delta_1s"))
    if not finite(tau) or tau < TAU_MIN or tau > TAU_MAX:
        return False
    if not finite(vwap):
        return False
    if not finite(edge) or edge <= EDGE_THRESHOLD:
        return False
    if not finite(fair_delta) or fair_delta <= 0:
        return False
    return True


def passes_rule(row: dict[str, Any], qty_label: str, version: str) -> bool:
    if not base_signal(row, qty_label):
        return False
    entry = number(row.get(vwap_field(qty_label)))
    edge = number(row.get(edge_field(qty_label)))
    if version == "V1":
        return True
    if version == "V2":
        return entry <= 0.45
    if version == "V3":
        return entry <= 0.40
    if version == "V4":
        return entry <= 0.40 and finite(edge / entry) and edge / entry >= 0.15
    raise ValueError(version)


def replay(rows: list[dict[str, Any]], qty_label: str, version: str) -> list[dict[str, Any]]:
    by_market: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_market.setdefault(str(row["market_slug"]), []).append(row)

    trades = []
    for market, market_rows in by_market.items():
        market_rows.sort(key=lambda row: number(row["recv_ts"]))
        for row in market_rows:
            if not passes_rule(row, qty_label, version):
                continue
            entry = number(row.get(vwap_field(qty_label)))
            edge = number(row.get(edge_field(qty_label)))
            payout = number(row.get("settlement_up"))
            pnl = payout - entry if finite(payout) and finite(entry) else math.nan
            trades.append({
                "market": market,
                "winner": row.get("settlement_winner", "unknown"),
                "entry_sample_index": int(number(row.get("sample_index"))),
                "entry_utc": row.get("recv_utc", ""),
                "tau_sec": number(row.get("tau_sec")),
                "entry": entry,
                "edge": edge,
                "relative_edge": edge / entry if finite(edge) and finite(entry) and entry > 0 else math.nan,
                "fair_up_sigma65": number(row.get("fair_up_sigma_floor")),
                "fair_sigma_floor_delta_1s": number(row.get("fair_sigma_floor_delta_1s")),
                "settlement_payout": payout,
                "settlement_pnl": pnl,
            })
            break
    trades.sort(key=lambda trade: trade["entry_sample_index"])
    return trades


def metrics(trades: list[dict[str, Any]]) -> dict[str, Any]:
    pnl = [number(trade.get("settlement_pnl")) for trade in trades]
    return {
        "trade_count": len(trades),
        "avg_entry": mean([number(trade.get("entry")) for trade in trades]),
        "avg_edge": mean([number(trade.get("edge")) for trade in trades]),
        "avg_relative_edge": mean([number(trade.get("relative_edge")) for trade in trades]),
        "settlement_mean": mean(pnl),
        "settlement_sum": sum(v for v in pnl if finite(v)),
        "settlement_p05": percentile(pnl, 0.05),
        "settlement_p25": percentile(pnl, 0.25),
        "settlement_p50": percentile(pnl, 0.50),
        "settlement_p75": percentile(pnl, 0.75),
        "settlement_p95": percentile(pnl, 0.95),
        "settlement_min": min([v for v in pnl if finite(v)], default=math.nan),
        "settlement_max": max([v for v in pnl if finite(v)], default=math.nan),
        "win_count": sum(1 for v in pnl if finite(v) and v > 0),
        "loss_count": sum(1 for v in pnl if finite(v) and v <= 0),
        "up_winning_trades": sum(1 for trade in trades if trade.get("winner") == "up"),
        "down_winning_trades": sum(1 for trade in trades if trade.get("winner") == "down"),
    }


def stress(trades: list[dict[str, Any]]) -> dict[str, Any]:
    out = {}
    pnl = [number(trade.get("settlement_pnl")) for trade in trades]
    for cost in (0.005, 0.010, 0.015, 0.020):
        stressed = [v - cost for v in pnl]
        out[f"{cost * 100:.1f}c"] = {
            "settlement_mean_after_cost": mean(stressed),
            "settlement_sum_after_cost": sum(v for v in stressed if finite(v)),
            "settlement_p05_after_cost": percentile(stressed, 0.05),
            "settlement_min_after_cost": min([v for v in stressed if finite(v)], default=math.nan),
            "win_count_after_cost": sum(1 for v in stressed if finite(v) and v > 0),
        }
    return out


def market_rows(all_markets: list[str], trades: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_market = {trade["market"]: trade for trade in trades}
    out = []
    for market in all_markets:
        trade = by_market.get(market)
        if trade is None:
            out.append({
                "market": market,
                "traded": False,
            })
        else:
            out.append({
                "market": market,
                "traded": True,
                "winner": trade["winner"],
                "entry_utc": trade["entry_utc"],
                "tau_sec": trade["tau_sec"],
                "entry": trade["entry"],
                "edge": trade["edge"],
                "relative_edge": trade["relative_edge"],
                "settlement_pnl": trade["settlement_pnl"],
            })
    return out


def build(rows: list[dict[str, Any]]) -> dict[str, Any]:
    all_markets = []
    seen = set()
    for row in rows:
        market = str(row["market_slug"])
        if market not in seen:
            seen.add(market)
            all_markets.append(market)

    result = {}
    for qty_label in ("q50", "q100"):
        result[qty_label] = {}
        for version in ("V1", "V2", "V3", "V4"):
            trades = replay(rows, qty_label, version)
            result[qty_label][version] = {
                "metrics": metrics(trades),
                "fee_slippage_stress": stress(trades),
                "trades": trades,
                "by_market": market_rows(all_markets, trades),
            }
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--run-dir",
        default="research/runs/btc5m_full_markets_20_20260630_061024_utc",
    )
    args = parser.parse_args()
    run_dir = Path(args.run_dir)
    rows = load_rows(run_dir / "samples_enriched.csv")
    attach_q100(rows, load_q100_vwap(run_dir / "clob_snapshots.jsonl"))
    out = {
        "run_dir": str(run_dir),
        "definitions": {
            "mode": "one trade max per market, first qualifying signal, hold to settlement",
            "V1": "60<=tau<=180 && edge_sigma65>3c && fair_sigma_floor_delta_1s>0 && VWAP exists",
            "V2": "V1 && entry<=45c",
            "V3": "V1 && entry<=40c",
            "V4": "V1 && entry<=40c && edge_sigma65/entry>=15%",
            "qty_handling": "q50 uses q50 VWAP/edge; q100 uses q100 VWAP/edge computed from raw CLOB",
        },
        "results": build(rows),
    }
    out_path = run_dir / "sigma65_buy_up_trade_rules_v1_v4.json"
    out_path.write_text(json.dumps(sanitize(out), indent=2), encoding="utf-8")
    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

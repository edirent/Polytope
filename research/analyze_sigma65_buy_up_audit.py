#!/usr/bin/env python3
"""Build focused sigma-floor BUY UP audit tables from a full BTC 5m capture."""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
from pathlib import Path
from typing import Any


TAU_BUCKETS = (
    ("240-300", 240.0, 300.000001),
    ("180-240", 180.0, 240.0),
    ("120-180", 120.0, 180.0),
    ("60-120", 60.0, 120.0),
    ("30-60", 30.0, 60.0),
    ("<30", -0.000001, 30.0),
)


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
        return [dict(row) for row in csv.DictReader(fh)]


def load_q100_vwap(path: Path) -> dict[int, float]:
    out: dict[int, float] = {}
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            obj = json.loads(line)
            idx = int(obj["sample_index"])
            out[idx] = vwap_buy(obj.get("up_asks") or [], 100.0)
    return out


def attach_q100(rows: list[dict[str, Any]], q100_vwap: dict[int, float]) -> None:
    by_market: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        idx = int(number(row["sample_index"]))
        vwap = q100_vwap.get(idx, math.nan)
        fair = number(row.get("fair_up_sigma_floor"))
        settlement = number(row.get("settlement_up"))
        row["vwap_buy_up_q100"] = vwap
        row["edge_buy_up_sigma_floor_q100"] = fair - vwap if finite(fair) and finite(vwap) else math.nan
        row["buy_up_settlement_pnl_q100"] = (
            settlement - vwap if finite(settlement) and finite(vwap) else math.nan
        )
        by_market.setdefault(str(row["market_slug"]), []).append(row)

    for market_rows in by_market.values():
        market_rows.sort(key=lambda row: number(row["recv_ts"]))
        times = [number(row["recv_ts"]) for row in market_rows]
        for row in market_rows:
            ts = number(row["recv_ts"])
            vwap = number(row.get("vwap_buy_up_q100"))
            for horizon in (3, 5):
                idx = bisect.bisect_left(times, ts + horizon)
                future = market_rows[idx] if idx < len(market_rows) else None
                future_mid = number(future.get("up_mid")) if future else math.nan
                row[f"buy_up_markout_{horizon}s_q100"] = (
                    future_mid - vwap if finite(future_mid) and finite(vwap) else math.nan
                )


def tau_bucket(row: dict[str, Any]) -> str:
    tau = number(row.get("tau_sec"))
    for name, lo, hi in TAU_BUCKETS:
        if tau >= lo and tau < hi:
            return name
    return "unknown"


def rule_rows(rows: list[dict[str, Any]], qty_label: str) -> list[dict[str, Any]]:
    edge_field = f"edge_buy_up_sigma_floor_{qty_label}"
    out = []
    for row in rows:
        fair_delta = number(row.get("fair_sigma_floor_delta_1s"))
        edge = number(row.get(edge_field))
        if not finite(fair_delta) or fair_delta <= 0:
            continue
        if not finite(edge) or edge <= 0.03:
            continue
        out.append(row)
    return out


def metrics(rows: list[dict[str, Any]], qty_label: str) -> dict[str, Any]:
    edge_field = f"edge_buy_up_sigma_floor_{qty_label}"
    m3_field = f"buy_up_markout_3s_{qty_label}"
    m5_field = f"buy_up_markout_5s_{qty_label}"
    settle_field = f"buy_up_settlement_pnl_{qty_label}"
    markets = sorted({str(row["market_slug"]) for row in rows})
    winners = [str(row.get("settlement_winner")) for row in rows]
    return {
        "count": len(rows),
        "market_count": len(markets),
        "avg_edge": mean([number(row.get(edge_field)) for row in rows]),
        "m3": mean([number(row.get(m3_field)) for row in rows]),
        "m5": mean([number(row.get(m5_field)) for row in rows]),
        "settlement": mean([number(row.get(settle_field)) for row in rows]),
        "positive_m3": sum(1 for row in rows if number(row.get(m3_field)) > 0),
        "positive_m5": sum(1 for row in rows if number(row.get(m5_field)) > 0),
        "positive_settlement": sum(1 for row in rows if number(row.get(settle_field)) > 0),
        "up_winning_signals": sum(1 for winner in winners if winner == "up"),
        "down_winning_signals": sum(1 for winner in winners if winner == "down"),
        "sample_markets": markets[:10],
    }


def stress_metrics(rows: list[dict[str, Any]], qty_label: str, cost: float) -> dict[str, Any]:
    edge_field = f"edge_buy_up_sigma_floor_{qty_label}"
    m3_field = f"buy_up_markout_3s_{qty_label}"
    m5_field = f"buy_up_markout_5s_{qty_label}"
    settle_field = f"buy_up_settlement_pnl_{qty_label}"
    edge = [number(row.get(edge_field)) - cost for row in rows]
    m3 = [number(row.get(m3_field)) - cost for row in rows]
    m5 = [number(row.get(m5_field)) - cost for row in rows]
    settlement = [number(row.get(settle_field)) - cost for row in rows]
    return {
        "cost": cost,
        "count": len(rows),
        "avg_edge_after_cost": mean(edge),
        "m3_after_cost": mean(m3),
        "m5_after_cost": mean(m5),
        "settlement_after_cost": mean(settlement),
        "positive_m3_after_cost": sum(1 for v in m3 if finite(v) and v > 0),
        "positive_m5_after_cost": sum(1 for v in m5 if finite(v) and v > 0),
        "positive_settlement_after_cost": sum(1 for v in settlement if finite(v) and v > 0),
        "settlement_min_after_cost": min([v for v in settlement if finite(v)], default=math.nan),
        "settlement_p05_after_cost": percentile(settlement, 0.05),
        "settlement_p25_after_cost": percentile(settlement, 0.25),
        "settlement_p50_after_cost": percentile(settlement, 0.50),
        "settlement_p75_after_cost": percentile(settlement, 0.75),
        "settlement_p95_after_cost": percentile(settlement, 0.95),
    }


def build_tables(rows: list[dict[str, Any]]) -> dict[str, Any]:
    base_qty = "q5"
    base = rule_rows(rows, base_qty)

    table1 = {}
    for bucket, _, _ in TAU_BUCKETS:
        bucket_rows = [row for row in base if tau_bucket(row) == bucket]
        table1[bucket] = metrics(bucket_rows, base_qty)

    table2 = {}
    for qty in ("q5", "q25", "q50", "q100"):
        qty_rows = rule_rows(rows, qty)
        table2[qty] = metrics(qty_rows, qty)

    table3 = {
        "UP-winning markets": metrics(
            [row for row in base if row.get("settlement_winner") == "up"],
            base_qty,
        ),
        "DOWN-winning markets": metrics(
            [row for row in base if row.get("settlement_winner") == "down"],
            base_qty,
        ),
    }

    market_order = []
    seen = set()
    for row in rows:
        slug = str(row["market_slug"])
        if slug not in seen:
            seen.add(slug)
            market_order.append(slug)

    table4 = []
    for slug in market_order:
        market_rows = [row for row in base if row["market_slug"] == slug]
        first = next(row for row in rows if row["market_slug"] == slug)
        row_metrics = metrics(market_rows, base_qty)
        table4.append({
            "market": slug,
            "winner": first.get("settlement_winner", "unknown"),
            "signal_count": row_metrics["count"],
            "avg_edge": row_metrics["avg_edge"],
            "m3": row_metrics["m3"],
            "m5": row_metrics["m5"],
            "settlement": row_metrics["settlement"],
        })

    table5 = {
        "base_before_cost": metrics(base, base_qty),
        "stress": {
            "0.5c": stress_metrics(base, base_qty, 0.005),
            "1.0c": stress_metrics(base, base_qty, 0.010),
            "1.5c": stress_metrics(base, base_qty, 0.015),
            "2.0c": stress_metrics(base, base_qty, 0.020),
        },
    }

    return {
        "definitions": {
            "rule": "buy_up && fair_sigma_floor_delta_1s > 0 && edge_buy_up_sigma_floor > 0.03",
            "base_qty_for_tables_1_3_4_5": "q5",
            "sigma_floor": 0.65,
            "markout": "future UP mid minus taker VWAP entry",
            "settlement": "official UP payout minus taker VWAP entry",
            "q100_source": "computed from clob_snapshots.jsonl up_asks",
        },
        "table1_sigma65_buy_up_by_tau": table1,
        "table2_sigma65_buy_up_by_qty": table2,
        "table3_sigma65_buy_up_by_outcome": table3,
        "table4_sigma65_buy_up_by_market": table4,
        "table5_fee_slippage_stress": table5,
    }


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
    tables = build_tables(rows)
    out = {
        "run_dir": str(run_dir),
        "row_count": len(rows),
        "tables": tables,
    }
    out_path = run_dir / "sigma65_buy_up_audit.json"
    out_path.write_text(json.dumps(sanitize(out), indent=2), encoding="utf-8")
    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

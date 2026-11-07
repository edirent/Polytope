#!/usr/bin/env python3
"""Focused 60-180s sigma65 BUY UP audit with regime and event clustering."""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
from pathlib import Path
from typing import Any


SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0
EDGE_THRESHOLD = 0.03
TAU_MIN = 60.0
TAU_MAX = 180.0
QTY_LABELS = ("q5", "q25", "q50", "q100")
STRESS_COSTS = (0.005, 0.010, 0.015, 0.020)


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


def safe_min(values: list[float]) -> float:
    clean = [v for v in values if finite(v)]
    return min(clean) if clean else math.nan


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
            out[int(obj["sample_index"])] = vwap_buy(obj.get("up_asks") or [], 100.0)
    return out


def attach_q100(rows: list[dict[str, Any]], q100_vwap: dict[int, float]) -> None:
    by_market: dict[str, list[dict[str, Any]]] = {}
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


def z_score_sigma65(row: dict[str, Any]) -> float:
    spot = number(row.get("spot_index_mid"))
    strike = number(row.get("round_start_price_k"))
    tau = max(0.0, number(row.get("tau_sec")))
    sigma = number(row.get("sigma_floor_annualized"), 0.65)
    if not (spot > 0 and strike > 0 and sigma > 0 and tau > 0):
        return math.nan
    tau_years = tau / SECONDS_PER_YEAR
    denom = sigma * math.sqrt(tau_years)
    if denom <= 0 or not finite(denom):
        return math.nan
    return math.log(spot / strike) / denom


def attach_regime_features(rows: list[dict[str, Any]]) -> None:
    for row in rows:
        spot = number(row.get("spot_index_mid"))
        strike = number(row.get("round_start_price_k"))
        row["spot_vs_K"] = (spot - strike) / strike if spot > 0 and strike > 0 else math.nan
        row["z_score_sigma65"] = z_score_sigma65(row)
        fair = number(row.get("fair_up_sigma_floor"))
        fair_delta_3s = number(row.get("fair_sigma_floor_delta_3s"))
        row["up_regime_filter"] = (
            (finite(fair) and fair > 0.55)
            or (finite(row["z_score_sigma65"]) and row["z_score_sigma65"] > 0.25)
            or (
                finite(row["spot_vs_K"])
                and row["spot_vs_K"] > 0
                and finite(fair_delta_3s)
                and fair_delta_3s > 0
            )
        )


def edge_field(qty_label: str) -> str:
    return f"edge_buy_up_sigma_floor_{qty_label}"


def m3_field(qty_label: str) -> str:
    return f"buy_up_markout_3s_{qty_label}"


def m5_field(qty_label: str) -> str:
    return f"buy_up_markout_5s_{qty_label}"


def settlement_field(qty_label: str) -> str:
    return f"buy_up_settlement_pnl_{qty_label}"


def vwap_field(qty_label: str) -> str:
    return f"vwap_buy_up_{qty_label}"


def is_signal(row: dict[str, Any], qty_label: str, use_regime_filter: bool = False) -> bool:
    tau = number(row.get("tau_sec"))
    if not finite(tau) or tau < TAU_MIN or tau > TAU_MAX:
        return False
    fair_delta = number(row.get("fair_sigma_floor_delta_1s"))
    edge = number(row.get(edge_field(qty_label)))
    if not finite(fair_delta) or fair_delta <= 0:
        return False
    if not finite(edge) or edge <= EDGE_THRESHOLD:
        return False
    if use_regime_filter and not row.get("up_regime_filter"):
        return False
    return True


def sample_metrics(rows: list[dict[str, Any]], qty_label: str) -> dict[str, Any]:
    edges = [number(row.get(edge_field(qty_label))) for row in rows]
    m3 = [number(row.get(m3_field(qty_label))) for row in rows]
    m5 = [number(row.get(m5_field(qty_label))) for row in rows]
    settlement = [number(row.get(settlement_field(qty_label))) for row in rows]
    markets = sorted({str(row.get("market_slug")) for row in rows})
    return {
        "count": len(rows),
        "market_count": len(markets),
        "avg_edge": mean(edges),
        "m3": mean(m3),
        "m5": mean(m5),
        "settlement_mean": mean(settlement),
        "settlement_p05": percentile(settlement, 0.05),
        "settlement_p25": percentile(settlement, 0.25),
        "settlement_p50": percentile(settlement, 0.50),
        "settlement_p75": percentile(settlement, 0.75),
        "settlement_p95": percentile(settlement, 0.95),
        "settlement_min": safe_min(settlement),
        "positive_m3": sum(1 for value in m3 if finite(value) and value > 0),
        "positive_m5": sum(1 for value in m5 if finite(value) and value > 0),
        "positive_settlement": sum(1 for value in settlement if finite(value) and value > 0),
        "up_winning_signals": sum(1 for row in rows if row.get("settlement_winner") == "up"),
        "down_winning_signals": sum(1 for row in rows if row.get("settlement_winner") == "down"),
    }


def stress_metrics(rows: list[dict[str, Any]], qty_label: str, cost: float) -> dict[str, Any]:
    edges = [number(row.get(edge_field(qty_label))) - cost for row in rows]
    m3 = [number(row.get(m3_field(qty_label))) - cost for row in rows]
    m5 = [number(row.get(m5_field(qty_label))) - cost for row in rows]
    settlement = [number(row.get(settlement_field(qty_label))) - cost for row in rows]
    return {
        "cost": cost,
        "count": len(rows),
        "avg_edge_after_cost": mean(edges),
        "m3_after_cost": mean(m3),
        "m5_after_cost": mean(m5),
        "settlement_mean_after_cost": mean(settlement),
        "settlement_p05_after_cost": percentile(settlement, 0.05),
        "settlement_min_after_cost": safe_min(settlement),
        "positive_m5_after_cost": sum(1 for value in m5 if finite(value) and value > 0),
        "positive_settlement_after_cost": sum(
            1 for value in settlement if finite(value) and value > 0
        ),
    }


def cluster_signals(signal_rows: list[dict[str, Any]], max_gap_sec: float = 1.5) -> list[list[dict[str, Any]]]:
    by_market: dict[str, list[dict[str, Any]]] = {}
    for row in signal_rows:
        by_market.setdefault(str(row["market_slug"]), []).append(row)

    clusters: list[list[dict[str, Any]]] = []
    for market_rows in by_market.values():
        market_rows.sort(key=lambda row: number(row["recv_ts"]))
        current: list[dict[str, Any]] = []
        prev_ts = math.nan
        for row in market_rows:
            ts = number(row["recv_ts"])
            if current and finite(prev_ts) and ts - prev_ts <= max_gap_sec:
                current.append(row)
            else:
                if current:
                    clusters.append(current)
                current = [row]
            prev_ts = ts
        if current:
            clusters.append(current)
    clusters.sort(key=lambda cluster: number(cluster[0]["recv_ts"]))
    return clusters


def best_vwap_row(cluster: list[dict[str, Any]], qty_label: str) -> dict[str, Any]:
    return min(
        cluster,
        key=lambda row: (
            number(row.get(vwap_field(qty_label)))
            if finite(number(row.get(vwap_field(qty_label))))
            else math.inf
        ),
    )


def event_rows_from_clusters(
    clusters: list[list[dict[str, Any]]],
    qty_label: str,
    entry_mode: str,
) -> list[dict[str, Any]]:
    out = []
    for event_id, cluster in enumerate(clusters):
        entry = cluster[0] if entry_mode == "first" else best_vwap_row(cluster, qty_label)
        out.append({
            "event_id": event_id,
            "market_slug": entry["market_slug"],
            "winner": entry.get("settlement_winner", "unknown"),
            "sample_count": len(cluster),
            "duration_sec": number(cluster[-1]["recv_ts"]) - number(cluster[0]["recv_ts"]),
            "entry_sample_index": entry["sample_index"],
            "entry_utc": entry["recv_utc"],
            "entry_tau_sec": number(entry.get("tau_sec")),
            "avg_edge": number(entry.get(edge_field(qty_label))),
            "m3": number(entry.get(m3_field(qty_label))),
            "m5": number(entry.get(m5_field(qty_label))),
            "settlement": number(entry.get(settlement_field(qty_label))),
        })
    return out


def event_metrics(events: list[dict[str, Any]]) -> dict[str, Any]:
    settlement = [number(event.get("settlement")) for event in events]
    return {
        "event_count": len(events),
        "sample_count_in_events": sum(int(event["sample_count"]) for event in events),
        "avg_event_duration_sec": mean([number(event.get("duration_sec")) for event in events]),
        "avg_edge": mean([number(event.get("avg_edge")) for event in events]),
        "m3": mean([number(event.get("m3")) for event in events]),
        "m5": mean([number(event.get("m5")) for event in events]),
        "settlement_mean": mean(settlement),
        "settlement_p05": percentile(settlement, 0.05),
        "settlement_min": safe_min(settlement),
        "positive_settlement": sum(1 for value in settlement if finite(value) and value > 0),
        "up_winning_events": sum(1 for event in events if event.get("winner") == "up"),
        "down_winning_events": sum(1 for event in events if event.get("winner") == "down"),
    }


def market_metrics(
    rows: list[dict[str, Any]],
    signal_rows: list[dict[str, Any]],
    events: list[dict[str, Any]],
    qty_label: str,
) -> list[dict[str, Any]]:
    market_order = []
    seen = set()
    for row in rows:
        slug = str(row["market_slug"])
        if slug not in seen:
            seen.add(slug)
            market_order.append(slug)

    by_event_market: dict[str, list[dict[str, Any]]] = {}
    for event in events:
        by_event_market.setdefault(str(event["market_slug"]), []).append(event)

    out = []
    for slug in market_order:
        market_rows = [row for row in signal_rows if row["market_slug"] == slug]
        first = next(row for row in rows if row["market_slug"] == slug)
        events_for_market = by_event_market.get(slug, [])
        settlement = [number(row.get(settlement_field(qty_label))) for row in market_rows]
        event_settlement = [number(event.get("settlement")) for event in events_for_market]
        out.append({
            "market": slug,
            "winner": first.get("settlement_winner", "unknown"),
            "signal_count": len(market_rows),
            "event_count": len(events_for_market),
            "avg_edge": mean([number(row.get(edge_field(qty_label))) for row in market_rows]),
            "m3": mean([number(row.get(m3_field(qty_label))) for row in market_rows]),
            "m5": mean([number(row.get(m5_field(qty_label))) for row in market_rows]),
            "sample_settlement_mean": mean(settlement),
            "sample_settlement_sum": sum(v for v in settlement if finite(v)),
            "sample_settlement_min": safe_min(settlement),
            "event_settlement_mean": mean(event_settlement),
            "event_settlement_sum": sum(v for v in event_settlement if finite(v)),
            "event_settlement_min": safe_min(event_settlement),
        })
    return out


def summarize_by_outcome(signal_rows: list[dict[str, Any]], qty_label: str) -> dict[str, Any]:
    return {
        "UP-winning markets": sample_metrics(
            [row for row in signal_rows if row.get("settlement_winner") == "up"],
            qty_label,
        ),
        "DOWN-winning markets": sample_metrics(
            [row for row in signal_rows if row.get("settlement_winner") == "down"],
            qty_label,
        ),
    }


def build_qty_section(rows: list[dict[str, Any]], qty_label: str, use_regime_filter: bool) -> dict[str, Any]:
    signals = [row for row in rows if is_signal(row, qty_label, use_regime_filter)]
    clusters = cluster_signals(signals)
    first_events = event_rows_from_clusters(clusters, qty_label, "first")
    best_events = event_rows_from_clusters(clusters, qty_label, "best_vwap")
    return {
        "sample_level": sample_metrics(signals, qty_label),
        "by_outcome": summarize_by_outcome(signals, qty_label),
        "event_level": {
            "cluster_gap_sec": 1.5,
            "entry_first_signal": event_metrics(first_events),
            "entry_best_vwap": event_metrics(best_events),
        },
        "market_level": market_metrics(rows, signals, first_events, qty_label),
        "fee_slippage_stress": {
            f"{cost * 100:.1f}c": stress_metrics(signals, qty_label, cost)
            for cost in STRESS_COSTS
        },
    }


def build_output(rows: list[dict[str, Any]]) -> dict[str, Any]:
    base = {qty: build_qty_section(rows, qty, False) for qty in QTY_LABELS}
    filtered = {qty: build_qty_section(rows, qty, True) for qty in QTY_LABELS}

    base_q5 = base["q5"]["sample_level"]
    filtered_q5 = filtered["q5"]["sample_level"]
    filter_effect = {
        "q5_before": base_q5,
        "q5_after": filtered_q5,
        "down_winning_signal_cut": (
            1.0 - filtered_q5["down_winning_signals"] / base_q5["down_winning_signals"]
            if base_q5["down_winning_signals"]
            else math.nan
        ),
        "up_winning_signal_retention": (
            filtered_q5["up_winning_signals"] / base_q5["up_winning_signals"]
            if base_q5["up_winning_signals"]
            else math.nan
        ),
    }
    return {
        "definitions": {
            "base_rule": (
                "buy_up && fair_sigma_floor_delta_1s > 0 && "
                "edge_buy_up_sigma_floor > 0.03 && 60 <= tau_sec <= 180"
            ),
            "up_regime_filter": (
                "fair_up_sigma65 > 0.55 OR z_score_sigma65 > 0.25 OR "
                "(spot > K AND fair_sigma_floor_delta_3s > 0)"
            ),
            "z_score_sigma65": "log(spot/K) / (0.65 * sqrt(tau_years))",
            "event_cluster": "same market consecutive signal rows with recv_ts gap <= 1.5s",
            "entry_first_signal": "first signal in cluster",
            "entry_best_vwap": "minimum BUY UP VWAP within cluster; optimistic diagnostic only",
            "q100_source": "computed from clob_snapshots.jsonl up_asks",
        },
        "base_60_180": base,
        "with_up_regime_filter": filtered,
        "up_regime_filter_effect_q5": filter_effect,
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
    attach_regime_features(rows)
    out = {
        "run_dir": str(run_dir),
        "row_count": len(rows),
        "analysis": build_output(rows),
    }
    out_path = run_dir / "sigma65_buy_up_60_180_audit.json"
    out_path.write_text(json.dumps(sanitize(out), indent=2), encoding="utf-8")
    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

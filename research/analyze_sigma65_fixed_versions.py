#!/usr/bin/env python3
"""Fixed-version BUY_UP q50 trade audit using raw CLOB asks for execution.

This script is intentionally narrower than the exploratory notebooks/scripts:
it replays only the approved fixed versions and derives entry_q50_vwap from
raw Polymarket UP asks in clob_snapshots.jsonl.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import statistics
from typing import Any

from analyze_sigma65_v2_trade_audit import (
    attach_regime_features,
    finite,
    group_by_market,
    load_rows,
    longest_loss_streak,
    max_drawdown,
    mean,
    median,
    number,
    percentile,
    safe_max,
    safe_min,
    sanitize,
)


TAU_MIN = 60.0
TAU_MAX = 180.0
EDGE_THRESHOLD = 0.03
ENTRY_CAP = 0.45
QTY = 50.0
COSTS = (0.0, 0.005, 0.01, 0.015, 0.02)


VERSION_RULES: dict[str, dict[str, Any]] = {
    "V2": {},
    "V2.1_spot_gt_K": {"spot_gt_K": True},
    "V2.2C_spot_gt_K_poly_d3_ge_0": {
        "spot_gt_K": True,
        "poly_up_mid_delta_3s_min": 0.0,
    },
    "V2.3a_V2.2C_z_gt_0": {
        "spot_gt_K": True,
        "poly_up_mid_delta_3s_min": 0.0,
        "z_gt": 0.0,
    },
    "V2.3b_V2.2C_z_gt_0p02": {
        "spot_gt_K": True,
        "poly_up_mid_delta_3s_min": 0.0,
        "z_gt": 0.02,
    },
    "V2.3c_V2.2C_z_gt_0p03": {
        "spot_gt_K": True,
        "poly_up_mid_delta_3s_min": 0.0,
        "z_gt": 0.03,
    },
    "V2.3d_V2.2C_z_gt_0p05": {
        "spot_gt_K": True,
        "poly_up_mid_delta_3s_min": 0.0,
        "z_gt": 0.05,
    },
    "V2.4_candidate_family": {
        "poly_up_mid_delta_3s_gt": 0.0,
        "v24_direction_family": True,
    },
}


def stddev(values: list[float]) -> float:
    clean = [value for value in values if finite(value)]
    return statistics.stdev(clean) if len(clean) >= 2 else math.nan


def load_clob_by_sample(path: Path) -> dict[int, dict[str, Any]]:
    out: dict[int, dict[str, Any]] = {}
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            obj = json.loads(line)
            sample_index = int(number(obj.get("sample_index")))
            out[sample_index] = obj
    return out


def consume_asks(asks: Any, qty: float) -> dict[str, Any]:
    if not isinstance(asks, list):
        return {
            "vwap": math.nan,
            "filled_qty": 0.0,
            "worst_price": math.nan,
            "used_levels": [],
        }
    levels: list[tuple[float, float]] = []
    for level in asks:
        if not isinstance(level, list | tuple) or len(level) < 2:
            continue
        price = number(level[0])
        size = number(level[1])
        if finite(price) and finite(size) and price > 0 and size > 0:
            levels.append((price, size))
    levels.sort(key=lambda item: item[0])

    remaining = qty
    notional = 0.0
    filled = 0.0
    used: list[dict[str, float]] = []
    worst_price = math.nan
    for price, size in levels:
        if remaining <= 1e-9:
            break
        take = min(size, remaining)
        if take <= 0:
            continue
        notional += price * take
        filled += take
        remaining -= take
        worst_price = price
        used.append({
            "price": price,
            "available_size": size,
            "used_qty": take,
        })

    return {
        "vwap": notional / filled if filled >= qty - 1e-9 else math.nan,
        "filled_qty": filled,
        "worst_price": worst_price if filled > 0 else math.nan,
        "used_levels": used,
    }


def raw_execution_for_row(
    row: dict[str, Any],
    clob_by_sample: dict[int, dict[str, Any]],
) -> dict[str, Any]:
    cached = row.get("_raw_q50_execution")
    if isinstance(cached, dict):
        return cached
    sample_index = int(number(row.get("sample_index")))
    clob = clob_by_sample.get(sample_index, {})
    execution = consume_asks(clob.get("up_asks"), QTY)
    csv_vwap = number(row.get("vwap_buy_up_q50"))
    execution["csv_vwap"] = csv_vwap
    execution["csv_vwap_diff"] = (
        execution["vwap"] - csv_vwap
        if finite(execution["vwap"]) and finite(csv_vwap)
        else math.nan
    )
    row["_raw_q50_execution"] = execution
    return execution


def row_signal_values(
    row: dict[str, Any],
    clob_by_sample: dict[int, dict[str, Any]],
) -> dict[str, Any]:
    execution = raw_execution_for_row(row, clob_by_sample)
    entry = number(execution.get("vwap"))
    fair = number(row.get("fair_up_sigma_floor"))
    edge = fair - entry if finite(fair) and finite(entry) else math.nan
    return {
        "entry": entry,
        "filled_qty": number(execution.get("filled_qty")),
        "edge": edge,
        "tau": number(row.get("tau_sec")),
        "fair_delta_1s": number(row.get("fair_sigma65_delta_1s_recalc")),
        "poly_up_mid_delta_3s": number(row.get("poly_up_mid_delta_3s_recalc")),
        "spot_return_3s": number(row.get("spot_return_3s_recalc")),
        "z_score_sigma65": number(row.get("z_score_sigma65")),
        "spot_gt_K": row.get("spot_gt_K") is True,
        "spot_vs_K_bps": number(row.get("spot_vs_K")) * 10000.0,
    }


def passes_base_signal(values: dict[str, Any]) -> bool:
    return (
        finite(values["tau"])
        and TAU_MIN <= values["tau"] <= TAU_MAX
        and finite(values["entry"])
        and values["filled_qty"] >= QTY - 1e-9
        and values["entry"] <= ENTRY_CAP
        and finite(values["edge"])
        and values["edge"] > EDGE_THRESHOLD
        and finite(values["fair_delta_1s"])
        and values["fair_delta_1s"] > 0
    )


def passes_version(values: dict[str, Any], filters: dict[str, Any]) -> bool:
    if filters.get("spot_gt_K") and not values["spot_gt_K"]:
        return False
    poly_min = filters.get("poly_up_mid_delta_3s_min")
    if poly_min is not None:
        poly_delta = values["poly_up_mid_delta_3s"]
        if not finite(poly_delta) or poly_delta < float(poly_min):
            return False
    poly_gt = filters.get("poly_up_mid_delta_3s_gt")
    if poly_gt is not None:
        poly_delta = values["poly_up_mid_delta_3s"]
        if not finite(poly_delta) or poly_delta <= float(poly_gt):
            return False
    z_gt = filters.get("z_gt")
    if z_gt is not None:
        z_score = values["z_score_sigma65"]
        if not finite(z_score) or z_score <= float(z_gt):
            return False
    if filters.get("v24_direction_family"):
        spot_return_3s = values["spot_return_3s"]
        if not (
            values["spot_gt_K"]
            or (finite(values["spot_vs_K_bps"]) and values["spot_vs_K_bps"] > -0.5)
            or (finite(spot_return_3s) and spot_return_3s > 0.0)
        ):
            return False
    return True


def trade_from_row(
    row: dict[str, Any],
    values: dict[str, Any],
    clob_by_sample: dict[int, dict[str, Any]],
) -> dict[str, Any]:
    execution = raw_execution_for_row(row, clob_by_sample)
    entry = values["entry"]
    settlement_up = number(row.get("settlement_up"))
    pnl = settlement_up - entry if finite(settlement_up) and finite(entry) else math.nan
    spot = number(row.get("spot_index_mid"))
    strike = number(row.get("round_start_price_k"))
    return {
        "market": row.get("market_slug", ""),
        "entry_ts": row.get("recv_utc", ""),
        "entry_recv_ts": number(row.get("recv_ts")),
        "sample_index": int(number(row.get("sample_index"))),
        "tau": values["tau"],
        "K": strike,
        "spot": spot,
        "spot_vs_K_bps": values["spot_vs_K_bps"],
        "z_score_sigma65": values["z_score_sigma65"],
        "fair_sigma65": number(row.get("fair_up_sigma_floor")),
        "fair_delta_1s": values["fair_delta_1s"],
        "poly_up_mid_delta_3s": values["poly_up_mid_delta_3s"],
        "spot_return_3s": values["spot_return_3s"],
        "entry_q50_vwap": entry,
        "entry_q50_vwap_csv": number(execution.get("csv_vwap")),
        "entry_q50_vwap_csv_diff": number(execution.get("csv_vwap_diff")),
        "entry_q50_filled_qty": values["filled_qty"],
        "entry_q50_worst_price": number(execution.get("worst_price")),
        "up_asks_used": execution.get("used_levels", []),
        "edge": values["edge"],
        "settlement_up": settlement_up,
        "settlement_winner": row.get("settlement_winner", "unknown"),
        "settlement_source": row.get("settlement_source", ""),
        "pnl": pnl,
    }


def replay_version(
    rows: list[dict[str, Any]],
    clob_by_sample: dict[int, dict[str, Any]],
    filters: dict[str, Any],
) -> list[dict[str, Any]]:
    trades = []
    for market_rows in group_by_market(rows).values():
        for row in market_rows:
            values = row_signal_values(row, clob_by_sample)
            if passes_base_signal(values) and passes_version(values, filters):
                trades.append(trade_from_row(row, values, clob_by_sample))
                break
    trades.sort(key=lambda trade: number(trade.get("entry_recv_ts")))
    return trades


def adjusted_pnls(trades: list[dict[str, Any]], cost: float) -> list[float]:
    out = []
    for trade in trades:
        pnl = number(trade.get("pnl"))
        out.append(pnl - cost if finite(pnl) else math.nan)
    return out


def metrics_for_cost(
    trades: list[dict[str, Any]],
    market_count: int,
    cost: float,
) -> dict[str, Any]:
    pnl = adjusted_pnls(trades, cost)
    trade_count = len(trades)
    return {
        "cost_per_share": cost,
        "markets": market_count,
        "trades": trade_count,
        "trade_rate": trade_count / market_count if market_count else math.nan,
        "win_rate": (
            sum(1 for value in pnl if finite(value) and value > 0) / trade_count
            if trade_count
            else math.nan
        ),
        "avg_entry": mean([number(trade.get("entry_q50_vwap")) for trade in trades]),
        "avg_edge": mean([number(trade.get("edge")) for trade in trades]),
        "mean_pnl": mean(pnl),
        "median_pnl": median(pnl),
        "p05": percentile(pnl, 0.05),
        "min": safe_min(pnl),
        "max": safe_max(pnl),
        "max_drawdown": max_drawdown(pnl),
        "longest_loss_streak": longest_loss_streak(pnl),
        "sum_pnl": sum(value for value in pnl if finite(value)),
        "up_winning_trades": sum(1 for trade in trades if trade.get("settlement_winner") == "up"),
        "down_winning_trades": sum(1 for trade in trades if trade.get("settlement_winner") == "down"),
    }


def bucket_start(ts: float, seconds: int) -> str:
    dt = datetime.fromtimestamp(ts, tz=timezone.utc)
    timestamp = int(dt.timestamp())
    start = timestamp - timestamp % seconds
    return datetime.fromtimestamp(start, tz=timezone.utc).isoformat()


def bucket_daily(ts: float) -> str:
    dt = datetime.fromtimestamp(ts, tz=timezone.utc)
    return datetime(dt.year, dt.month, dt.day, tzinfo=timezone.utc).isoformat()


def pnl_buckets(
    trades: list[dict[str, Any]],
    cost: float,
    bucket_fn: Any,
) -> list[dict[str, Any]]:
    buckets: dict[str, dict[str, Any]] = {}
    for trade in trades:
        ts = number(trade.get("entry_recv_ts"))
        pnl = number(trade.get("pnl"))
        if not finite(ts) or not finite(pnl):
            continue
        key = bucket_fn(ts)
        bucket = buckets.setdefault(key, {"bucket_start_utc": key, "trade_count": 0, "pnl": 0.0})
        bucket["trade_count"] += 1
        bucket["pnl"] += pnl - cost
    return [buckets[key] for key in sorted(buckets)]


def bucket_stats(buckets: list[dict[str, Any]]) -> dict[str, Any]:
    pnls = [number(bucket.get("pnl")) for bucket in buckets]
    return {
        "bucket_count": len(buckets),
        "mean_pnl": mean(pnls),
        "std_pnl": stddev(pnls),
        "positive_bucket_pct": (
            sum(1 for value in pnls if finite(value) and value > 0) / len(pnls)
            if pnls
            else math.nan
        ),
        "max_losing_bucket": safe_min(pnls),
        "max_drawdown": max_drawdown(pnls),
    }


def time_distribution(trades: list[dict[str, Any]], cost: float) -> dict[str, Any]:
    hourly = pnl_buckets(trades, cost, lambda ts: bucket_start(ts, 3600))
    four_hour = pnl_buckets(trades, cost, lambda ts: bucket_start(ts, 4 * 3600))
    daily = pnl_buckets(trades, cost, bucket_daily)
    daily_pnls = [number(bucket.get("pnl")) for bucket in daily]
    daily_std = stddev(daily_pnls)
    daily_sharpe = (
        mean(daily_pnls) / daily_std * math.sqrt(365.0)
        if finite(daily_std) and daily_std > 0
        else math.nan
    )
    return {
        "cost_per_share": cost,
        "hourly": {
            "stats": bucket_stats(hourly) | {
                "mean_hourly_pnl": bucket_stats(hourly)["mean_pnl"],
                "std_hourly_pnl": bucket_stats(hourly)["std_pnl"],
                "positive_hour_pct": bucket_stats(hourly)["positive_bucket_pct"],
                "max_losing_hour": bucket_stats(hourly)["max_losing_bucket"],
            },
            "buckets": hourly,
        },
        "four_hour": {
            "stats": bucket_stats(four_hour),
            "buckets": four_hour,
        },
        "daily": {
            "stats": bucket_stats(daily) | {"daily_sharpe_approx": daily_sharpe},
            "buckets": daily,
        },
    }


def split_by_winner(trades: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    out = {}
    for winner in ("up", "down"):
        group = [trade for trade in trades if trade.get("settlement_winner") == winner]
        out[f"{winner}_winning_trades"] = {
            "count": len(group),
            "mean_pnl": mean([number(trade.get("pnl")) for trade in group]),
            "avg_entry": mean([number(trade.get("entry_q50_vwap")) for trade in group]),
            "avg_z": mean([number(trade.get("z_score_sigma65")) for trade in group]),
            "avg_spot_bps": mean([number(trade.get("spot_vs_K_bps")) for trade in group]),
        }
    up_count = out["up_winning_trades"]["count"]
    down_count = out["down_winning_trades"]["count"]
    out["up_down_count_ratio"] = up_count / down_count if down_count else math.inf
    return out


def execution_checks(trades: list[dict[str, Any]]) -> dict[str, Any]:
    diffs = [abs(number(trade.get("entry_q50_vwap_csv_diff"))) for trade in trades]
    finite_diffs = [diff for diff in diffs if finite(diff)]
    return {
        "entry_source": "raw clob_snapshots.jsonl up_asks",
        "not_used_for_entry": [
            "down_bid",
            "1 - down_bid",
            "future_mid",
            "settlement",
        ],
        "trades_with_raw_q50_fill": sum(
            1 for trade in trades if number(trade.get("entry_q50_filled_qty")) >= QTY - 1e-9
        ),
        "max_abs_csv_vwap_diff": max(finite_diffs) if finite_diffs else math.nan,
        "csv_vwap_diff_gt_0p0001": sum(1 for diff in finite_diffs if diff > 0.0001),
    }


def version_report(
    trades: list[dict[str, Any]],
    market_count: int,
    filters: dict[str, Any],
) -> dict[str, Any]:
    return {
        "filters": filters,
        "summary": metrics_for_cost(trades, market_count, 0.0),
        "cost_stress": {
            f"cost_{cost:.4f}": metrics_for_cost(trades, market_count, cost)
            for cost in COSTS
        },
        "time_distribution_by_cost": {
            f"cost_{cost:.4f}": time_distribution(trades, cost)
            for cost in COSTS
        },
        "winning_split": split_by_winner(trades),
        "execution_checks": execution_checks(trades),
        "trades": trades,
    }


def write_markdown_report(path: Path, data: dict[str, Any]) -> None:
    def cents(value: Any) -> str:
        n = number(value)
        return "NA" if not finite(n) else f"{n * 100:.2f}c"

    lines = [
        "# Sigma65 Fixed-Version Trade Audit",
        "",
        f"- market_count: {data['market_count']}",
        f"- samples_file: `{data['samples_file']}`",
        f"- clob_file: `{data['clob_file']}`",
        "- entry source: raw `up_asks` from `clob_snapshots.jsonl`",
        "- forbidden entry sources: `1 - down_bid`, future mid, settlement",
        "",
        "## Version Summary",
        "",
        "| version | trades | win | mean | median | p05 | min | drawdown | loss streak | UP/DOWN |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name, version in data["versions"].items():
        summary = version["summary"]
        split = version["winning_split"]
        up_count = split["up_winning_trades"]["count"]
        down_count = split["down_winning_trades"]["count"]
        win_rate = number(summary.get("win_rate"))
        lines.append(
            "| "
            + " | ".join([
                name,
                str(summary["trades"]),
                "NA" if not finite(win_rate) else f"{win_rate * 100:.1f}%",
                cents(summary["mean_pnl"]),
                cents(summary["median_pnl"]),
                cents(summary["p05"]),
                cents(summary["min"]),
                cents(summary["max_drawdown"]),
                str(summary["longest_loss_streak"]),
                f"{up_count}/{down_count}",
            ])
            + " |"
        )
    lines.extend([
        "",
        "## V2.2C Cost Stress",
        "",
        "| cost | mean | p05 | min | drawdown | loss streak | sum |",
        "|---:|---:|---:|---:|---:|---:|---:|",
    ])
    v22c = data["versions"].get("V2.2C_spot_gt_K_poly_d3_ge_0", {})
    for cost_key, metrics in v22c.get("cost_stress", {}).items():
        lines.append(
            "| "
            + " | ".join([
                cents(metrics["cost_per_share"]),
                cents(metrics["mean_pnl"]),
                cents(metrics["p05"]),
                cents(metrics["min"]),
                cents(metrics["max_drawdown"]),
                str(metrics["longest_loss_streak"]),
                cents(metrics["sum_pnl"]),
            ])
            + " |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--samples-file", default="samples_enriched.csv")
    parser.add_argument("--clob-file", default="clob_snapshots.jsonl")
    parser.add_argument("--out-file", default="sigma65_fixed_versions_trade_audit.json")
    parser.add_argument("--report-file", default="sigma65_fixed_versions_trade_audit.md")
    args = parser.parse_args()

    run_dir = Path(args.run_dir)
    samples_file = Path(args.samples_file)
    if not samples_file.is_absolute():
        samples_file = run_dir / samples_file
    clob_file = Path(args.clob_file)
    if not clob_file.is_absolute():
        clob_file = run_dir / clob_file

    rows = load_rows(samples_file)
    attach_regime_features(rows)
    clob_by_sample = load_clob_by_sample(clob_file)
    market_count = len({row["market_slug"] for row in rows})

    data = {
        "run_dir": str(run_dir),
        "samples_file": str(samples_file),
        "clob_file": str(clob_file),
        "market_count": market_count,
        "base_rule": {
            "side": "BUY_UP",
            "qty": QTY,
            "tau": "60 <= tau <= 180",
            "edge": "fair_sigma65 - raw_up_asks_q50_vwap > 0.03",
            "fair_delta": "fair_up_sigma_floor[t] - fair_up_sigma_floor[t-1s] > 0",
            "entry_cap": "raw_up_asks_q50_vwap <= 0.45",
            "execution": "first qualifying signal per market, hold to settlement",
        },
        "execution_realism": {
            "entry_q50_vwap_source": "raw up_asks from clob_snapshots.jsonl",
            "entry_q50_vwap_not_from": [
                "1 - down_bid",
                "down_bid",
                "future_mid",
                "settlement",
            ],
            "settlement_used_only_for": "post-trade PnL",
            "fair_and_momentum_sources": "current row and prior rows only",
        },
        "versions": {},
    }

    for version, filters in VERSION_RULES.items():
        trades = replay_version(rows, clob_by_sample, filters)
        data["versions"][version] = version_report(trades, market_count, filters)

    out_path = Path(args.out_file)
    if not out_path.is_absolute():
        out_path = run_dir / out_path
    out_path.write_text(json.dumps(sanitize(data), indent=2), encoding="utf-8")

    report_path = Path(args.report_file)
    if not report_path.is_absolute():
        report_path = run_dir / report_path
    write_markdown_report(report_path, sanitize(data))

    print(out_path)
    print(report_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

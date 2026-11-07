#!/usr/bin/env python3
"""Trade-level audit for sigma65 BUY UP V2/V2.1.

V2:
  BUY UP q50
  60 <= tau <= 180
  edge_sigma65_q50 > 3c
  fair_sigma_floor_delta_1s > 0
  q50 VWAP exists
  entry_q50 <= 45c
  one trade max per market, first qualifying entry, hold to settlement.

V2.1 adds:
  spot > K

V2.2 experiments add optional threshold filters on spot_vs_K bps, z score,
and Polymarket UP mid momentum.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
from pathlib import Path
from typing import Any


SECONDS_PER_YEAR = 365.0 * 24.0 * 60.0 * 60.0
TAU_MIN = 60.0
TAU_MAX = 180.0
EDGE_THRESHOLD = 0.03
ENTRY_CAP = 0.45


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
    if len(clean) == 1:
        return clean[0]
    pos = (len(clean) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return clean[lo]
    weight = pos - lo
    return clean[lo] * (1.0 - weight) + clean[hi] * weight


def median(values: list[float]) -> float:
    return percentile(values, 0.50)


def safe_min(values: list[float]) -> float:
    clean = [v for v in values if finite(v)]
    return min(clean) if clean else math.nan


def safe_max(values: list[float]) -> float:
    clean = [v for v in values if finite(v)]
    return max(clean) if clean else math.nan


def sanitize(value: Any) -> Any:
    if isinstance(value, float):
        return value if finite(value) else None
    if isinstance(value, dict):
        return {str(k): sanitize(v) for k, v in value.items()}
    if isinstance(value, list):
        return [sanitize(v) for v in value]
    return value


def load_rows(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="", encoding="utf-8") as fh:
        rows = [dict(row) for row in csv.DictReader(fh)]
    rows.sort(key=lambda row: number(row["recv_ts"]))
    return rows


def group_by_market(rows: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    out: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        out.setdefault(str(row["market_slug"]), []).append(row)
    for market_rows in out.values():
        market_rows.sort(key=lambda row: number(row["recv_ts"]))
    return out


def find_at_or_before(rows: list[dict[str, Any]], times: list[float], target_ts: float) -> dict[str, Any] | None:
    idx = bisect.bisect_right(times, target_ts) - 1
    return rows[idx] if idx >= 0 else None


def delta(now_value: Any, prev_value: Any) -> float:
    now = number(now_value)
    prev = number(prev_value)
    return now - prev if finite(now) and finite(prev) else math.nan


def ret(now_value: Any, prev_value: Any) -> float:
    now = number(now_value)
    prev = number(prev_value)
    if not finite(now) or not finite(prev) or prev <= 0:
        return math.nan
    return now / prev - 1.0


def z_score_sigma65(row: dict[str, Any]) -> float:
    spot = number(row.get("spot_index_mid"))
    strike = number(row.get("round_start_price_k"))
    tau = number(row.get("tau_sec"))
    sigma = number(row.get("sigma_floor_annualized"), 0.65)
    if not (spot > 0 and strike > 0 and tau > 0 and sigma > 0):
        return math.nan
    denom = sigma * math.sqrt(tau / SECONDS_PER_YEAR)
    if denom <= 0 or not finite(denom):
        return math.nan
    return math.log(spot / strike) / denom


def attach_regime_features(rows: list[dict[str, Any]]) -> None:
    for market_rows in group_by_market(rows).values():
        times = [number(row["recv_ts"]) for row in market_rows]
        for row in market_rows:
            ts = number(row["recv_ts"])
            spot = number(row.get("spot_index_mid"))
            strike = number(row.get("round_start_price_k"))
            row["spot_vs_K"] = (spot - strike) / strike if spot > 0 and strike > 0 else math.nan
            row["spot_gt_K"] = bool(finite(row["spot_vs_K"]) and row["spot_vs_K"] > 0)
            row["z_score_sigma65"] = z_score_sigma65(row)
            for horizon in (1, 3, 5, 10):
                prev = find_at_or_before(market_rows, times, ts - horizon)
                row[f"spot_return_{horizon}s_recalc"] = (
                    ret(row.get("spot_index_mid"), prev.get("spot_index_mid"))
                    if prev is not None
                    else math.nan
                )
                row[f"fair_sigma65_delta_{horizon}s_recalc"] = (
                    delta(row.get("fair_up_sigma_floor"), prev.get("fair_up_sigma_floor"))
                    if prev is not None
                    else math.nan
                )
                row[f"poly_up_mid_delta_{horizon}s_recalc"] = (
                    delta(row.get("up_mid"), prev.get("up_mid"))
                    if prev is not None
                    else math.nan
                )


def is_v2_signal(
    row: dict[str, Any],
    require_spot_gt_k: bool = False,
    min_spot_vs_k_bps: float | None = None,
    min_z_score: float | None = None,
    min_poly_up_mid_delta_3s: float | None = None,
    gt_poly_up_mid_delta_3s: float | None = None,
) -> bool:
    tau = number(row.get("tau_sec"))
    entry = number(row.get("vwap_buy_up_q50"))
    edge = number(row.get("edge_buy_up_sigma_floor_q50"))
    fair_delta = number(row.get("fair_sigma_floor_delta_1s"))
    if require_spot_gt_k and row.get("spot_gt_K") is not True:
        return False
    if min_spot_vs_k_bps is not None:
        spot_vs_k_bps = number(row.get("spot_vs_K")) * 10000.0
        if not finite(spot_vs_k_bps) or spot_vs_k_bps < min_spot_vs_k_bps:
            return False
    if min_z_score is not None:
        z_score = number(row.get("z_score_sigma65"))
        if not finite(z_score) or z_score <= min_z_score:
            return False
    if min_poly_up_mid_delta_3s is not None:
        poly_delta = number(row.get("poly_up_mid_delta_3s_recalc"))
        if not finite(poly_delta) or poly_delta < min_poly_up_mid_delta_3s:
            return False
    if gt_poly_up_mid_delta_3s is not None:
        poly_delta = number(row.get("poly_up_mid_delta_3s_recalc"))
        if not finite(poly_delta) or poly_delta <= gt_poly_up_mid_delta_3s:
            return False
    if not finite(tau) or tau < TAU_MIN or tau > TAU_MAX:
        return False
    if not finite(entry) or entry > ENTRY_CAP:
        return False
    if not finite(edge) or edge <= EDGE_THRESHOLD:
        return False
    if not finite(fair_delta) or fair_delta <= 0:
        return False
    return True


def trade_from_row(row: dict[str, Any]) -> dict[str, Any]:
    entry = number(row.get("vwap_buy_up_q50"))
    payout = number(row.get("settlement_up"))
    edge = number(row.get("edge_buy_up_sigma_floor_q50"))
    pnl = payout - entry if finite(payout) and finite(entry) else math.nan
    return {
        "market": row["market_slug"],
        "entry_time": row.get("recv_utc", ""),
        "entry_sample_index": int(number(row.get("sample_index"))),
        "winner": row.get("settlement_winner", "unknown"),
        "tau": number(row.get("tau_sec")),
        "entry_price": entry,
        "edge": edge,
        "relative_edge": edge / entry if finite(edge) and finite(entry) and entry > 0 else math.nan,
        "fair_up_sigma65": number(row.get("fair_up_sigma_floor")),
        "z_score_sigma65": number(row.get("z_score_sigma65")),
        "spot_gt_K": row.get("spot_gt_K"),
        "spot_vs_K": number(row.get("spot_vs_K")),
        "spot_return_1s": number(row.get("spot_return_1s_recalc")),
        "spot_return_3s": number(row.get("spot_return_3s_recalc")),
        "spot_return_5s": number(row.get("spot_return_5s_recalc")),
        "spot_return_10s": number(row.get("spot_return_10s_recalc")),
        "fair_delta_1s": number(row.get("fair_sigma65_delta_1s_recalc")),
        "fair_delta_3s": number(row.get("fair_sigma65_delta_3s_recalc")),
        "fair_delta_5s": number(row.get("fair_sigma65_delta_5s_recalc")),
        "fair_delta_10s": number(row.get("fair_sigma65_delta_10s_recalc")),
        "poly_up_mid_delta_1s": number(row.get("poly_up_mid_delta_1s_recalc")),
        "poly_up_mid_delta_3s": number(row.get("poly_up_mid_delta_3s_recalc")),
        "poly_up_mid_delta_5s": number(row.get("poly_up_mid_delta_5s_recalc")),
        "poly_up_mid_delta_10s": number(row.get("poly_up_mid_delta_10s_recalc")),
        "settlement_payout": payout,
        "settlement_pnl": pnl,
    }


def replay_v2(
    rows: list[dict[str, Any]],
    require_spot_gt_k: bool = False,
    min_spot_vs_k_bps: float | None = None,
    min_z_score: float | None = None,
    min_poly_up_mid_delta_3s: float | None = None,
    gt_poly_up_mid_delta_3s: float | None = None,
) -> list[dict[str, Any]]:
    trades = []
    for market_rows in group_by_market(rows).values():
        for row in market_rows:
            if is_v2_signal(
                row,
                require_spot_gt_k=require_spot_gt_k,
                min_spot_vs_k_bps=min_spot_vs_k_bps,
                min_z_score=min_z_score,
                min_poly_up_mid_delta_3s=min_poly_up_mid_delta_3s,
                gt_poly_up_mid_delta_3s=gt_poly_up_mid_delta_3s,
            ):
                trades.append(trade_from_row(row))
                break
    trades.sort(key=lambda trade: trade["entry_sample_index"])
    return trades


def max_drawdown(pnl: list[float]) -> float:
    equity = 0.0
    peak = 0.0
    max_dd = 0.0
    for value in pnl:
        if not finite(value):
            continue
        equity += value
        peak = max(peak, equity)
        max_dd = max(max_dd, peak - equity)
    return max_dd


def longest_loss_streak(pnl: list[float]) -> int:
    current = 0
    longest = 0
    for value in pnl:
        if finite(value) and value <= 0:
            current += 1
            longest = max(longest, current)
        else:
            current = 0
    return longest


def metrics(trades: list[dict[str, Any]], market_count: int) -> dict[str, Any]:
    pnl = [number(trade.get("settlement_pnl")) for trade in trades]
    trade_count = len(trades)
    out = {
        "markets": market_count,
        "trades": trade_count,
        "trade_rate": trade_count / market_count if market_count else math.nan,
        "win_rate": (
            sum(1 for value in pnl if finite(value) and value > 0) / trade_count
            if trade_count
            else math.nan
        ),
        "avg_entry": mean([number(trade.get("entry_price")) for trade in trades]),
        "avg_edge": mean([number(trade.get("edge")) for trade in trades]),
        "mean_pnl": mean(pnl),
        "median_pnl": median(pnl),
        "p05": percentile(pnl, 0.05),
        "min": safe_min(pnl),
        "max": safe_max(pnl),
        "max_drawdown": max_drawdown(pnl),
        "longest_loss_streak": longest_loss_streak(pnl),
        "sum_pnl": sum(value for value in pnl if finite(value)),
        "up_winning_trades": sum(1 for trade in trades if trade.get("winner") == "up"),
        "down_winning_trades": sum(1 for trade in trades if trade.get("winner") == "down"),
    }
    criteria = {
        "trades_gte_40": out["trades"] >= 40,
        "mean_pnl_gt_15c": finite(out["mean_pnl"]) and out["mean_pnl"] > 0.15,
        "p05_gt_minus_25c": finite(out["p05"]) and out["p05"] > -0.25,
        "min_gt_minus_35c": finite(out["min"]) and out["min"] > -0.35,
        "longest_loss_streak_lte_4": out["longest_loss_streak"] <= 4,
    }
    out["criteria"] = criteria
    out["passes"] = all(criteria.values())
    return out


def bucket(value: float, cuts: list[tuple[str, float, float]]) -> str:
    if not finite(value):
        return "missing"
    for name, lo, hi in cuts:
        if value >= lo and value < hi:
            return name
    return "other"


def summarize_trade_subset(trades: list[dict[str, Any]]) -> dict[str, Any]:
    pnl = [number(trade.get("settlement_pnl")) for trade in trades]
    return {
        "count": len(trades),
        "mean_pnl": mean(pnl),
        "median_pnl": median(pnl),
        "p05": percentile(pnl, 0.05),
        "min": safe_min(pnl),
        "win_rate": (
            sum(1 for value in pnl if finite(value) and value > 0) / len(trades)
            if trades
            else math.nan
        ),
        "avg_entry": mean([number(trade.get("entry_price")) for trade in trades]),
        "avg_edge": mean([number(trade.get("edge")) for trade in trades]),
        "up_winning": sum(1 for trade in trades if trade.get("winner") == "up"),
        "down_winning": sum(1 for trade in trades if trade.get("winner") == "down"),
    }


def split_by(trades: list[dict[str, Any]], name: str, fn: Any) -> dict[str, Any]:
    groups: dict[str, list[dict[str, Any]]] = {}
    for trade in trades:
        groups.setdefault(str(fn(trade)), []).append(trade)
    return {key: summarize_trade_subset(value) for key, value in sorted(groups.items())}


def regime_summary(trades: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "spot_gt_K": split_by(trades, "spot_gt_K", lambda t: t.get("spot_gt_K")),
        "winner": split_by(trades, "winner", lambda t: t.get("winner")),
        "entry_bucket": split_by(
            trades,
            "entry_bucket",
            lambda t: bucket(number(t.get("entry_price")), [
                ("entry_lt_0p15", -math.inf, 0.15),
                ("entry_0p15_0p25", 0.15, 0.25),
                ("entry_0p25_0p35", 0.25, 0.35),
                ("entry_0p35_0p45", 0.35, 0.450000001),
            ]),
        ),
        "z_score_bucket": split_by(
            trades,
            "z_score_bucket",
            lambda t: bucket(number(t.get("z_score_sigma65")), [
                ("z_lt_0", -math.inf, 0.0),
                ("z_0_0p25", 0.0, 0.25),
                ("z_0p25_0p5", 0.25, 0.5),
                ("z_gt_0p5", 0.5, math.inf),
            ]),
        ),
        "fair_up_bucket": split_by(
            trades,
            "fair_up_bucket",
            lambda t: bucket(number(t.get("fair_up_sigma65")), [
                ("fair_lt_0p45", -math.inf, 0.45),
                ("fair_0p45_0p55", 0.45, 0.55),
                ("fair_0p55_0p65", 0.55, 0.65),
                ("fair_gt_0p65", 0.65, math.inf),
            ]),
        ),
        "spot_return_3s_sign": split_by(
            trades,
            "spot_return_3s_sign",
            lambda t: "positive" if number(t.get("spot_return_3s")) > 0 else "non_positive_or_missing",
        ),
        "poly_up_mid_delta_3s_sign": split_by(
            trades,
            "poly_up_mid_delta_3s_sign",
            lambda t: "positive" if number(t.get("poly_up_mid_delta_3s")) > 0 else "non_positive_or_missing",
        ),
        "fair_delta_3s_sign": split_by(
            trades,
            "fair_delta_3s_sign",
            lambda t: "positive" if number(t.get("fair_delta_3s")) > 0 else "non_positive_or_missing",
        ),
    }


def loss_audit(trades: list[dict[str, Any]]) -> list[dict[str, Any]]:
    losses = [trade for trade in trades if number(trade.get("settlement_pnl")) <= 0]
    losses.sort(key=lambda trade: number(trade.get("settlement_pnl")))
    fields = [
        "market",
        "entry_time",
        "tau",
        "entry_price",
        "edge",
        "fair_up_sigma65",
        "z_score_sigma65",
        "spot_vs_K",
        "spot_return_1s",
        "spot_return_3s",
        "spot_return_5s",
        "fair_delta_1s",
        "fair_delta_3s",
        "fair_delta_5s",
        "poly_up_mid_delta_1s",
        "poly_up_mid_delta_3s",
        "poly_up_mid_delta_5s",
        "settlement_pnl",
    ]
    return [{field: trade.get(field) for field in fields} for trade in losses]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--run-dir",
        default="research/runs/btc5m_full_markets_20_20260630_061024_utc",
    )
    parser.add_argument("--samples-file", default="samples_enriched.csv")
    parser.add_argument("--out-file", default="sigma65_v2_q50_trade_audit.json")
    parser.add_argument("--require-spot-gt-k", action="store_true")
    parser.add_argument("--min-spot-vs-k-bps", type=float)
    parser.add_argument("--min-z-score", type=float)
    parser.add_argument("--min-poly-up-mid-delta-3s", type=float)
    parser.add_argument("--gt-poly-up-mid-delta-3s", type=float)
    args = parser.parse_args()

    run_dir = Path(args.run_dir)
    samples_file = Path(args.samples_file)
    if not samples_file.is_absolute():
        samples_file = run_dir / samples_file
    rows = load_rows(samples_file)
    attach_regime_features(rows)
    market_count = len({row["market_slug"] for row in rows})
    trades = replay_v2(
        rows,
        require_spot_gt_k=args.require_spot_gt_k,
        min_spot_vs_k_bps=args.min_spot_vs_k_bps,
        min_z_score=args.min_z_score,
        min_poly_up_mid_delta_3s=args.min_poly_up_mid_delta_3s,
        gt_poly_up_mid_delta_3s=args.gt_poly_up_mid_delta_3s,
    )
    out = {
        "run_dir": str(run_dir),
        "rule": {
            "version": "V2.2" if any([
                args.min_spot_vs_k_bps is not None,
                args.min_z_score is not None,
                args.min_poly_up_mid_delta_3s is not None,
                args.gt_poly_up_mid_delta_3s is not None,
            ]) else ("V2.1" if args.require_spot_gt_k else "V2"),
            "side": "BUY_UP",
            "qty": 50,
            "tau": "60 <= tau <= 180",
            "edge": "edge_sigma65_q50 > 0.03",
            "fair_delta": "fair_sigma_floor_delta_1s > 0",
            "entry_cap": "entry_q50 <= 0.45",
            "spot_gt_K": args.require_spot_gt_k,
            "min_spot_vs_K_bps": args.min_spot_vs_k_bps,
            "min_z_score_sigma65": args.min_z_score,
            "min_poly_up_mid_delta_3s": args.min_poly_up_mid_delta_3s,
            "gt_poly_up_mid_delta_3s": args.gt_poly_up_mid_delta_3s,
            "execution": "first qualifying signal per market, hold to settlement",
        },
        "summary": metrics(trades, market_count),
        "by_market_regime": regime_summary(trades),
        "loss_audit": loss_audit(trades),
        "trades": trades,
    }
    out_path = Path(args.out_file)
    if not out_path.is_absolute():
        out_path = run_dir / out_path
    out_path.write_text(json.dumps(sanitize(out), indent=2), encoding="utf-8")
    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

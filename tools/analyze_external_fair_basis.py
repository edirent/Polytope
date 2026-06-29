#!/usr/bin/env python3
import argparse
import bisect
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean, median


DEFAULT_HORIZONS = [10, 30, 60, 300, 900, 1800]
DEFAULT_PRICE_SCALE_TICK = 1_000_000
DEFAULT_MAX_SPREAD_CENTS = 0.5
DEFAULT_EDGE_THRESHOLD_CENTS = [0.5, 1.0, 2.0]


def cents_to_tick(cents, price_scale_tick):
    return round(price_scale_tick * cents / 100.0)


def edge_signal_name(side, cents):
    if cents == 0.5:
        suffix = "0p5c"
    elif cents == 1.0:
        suffix = "1p0c"
    elif cents == 2.0:
        suffix = "2p0c"
    else:
        suffix = f"{cents:g}".replace(".", "p") + "c"
    return f"{side}_edge_{suffix}"


def edge_frequency_column(side, cents):
    return f"{edge_signal_name(side, cents)}_rate"


def to_int(row, key, default=0):
    value = row.get(key, "")
    if value == "":
        return default
    return int(float(value))


def to_float(row, key, default=0.0):
    value = row.get(key, "")
    if value == "":
        return default
    return float(value)


def percentile(values, p):
    if not values:
        return ""
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * p
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return ordered[lo]
    weight = rank - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight


def rate(rows, predicate):
    if not rows:
        return ""
    return sum(1 for row in rows if predicate(row)) / len(rows)


def avg(rows, key):
    values = [to_float(row, key) for row in rows if row.get(key, "") != ""]
    return mean(values) if values else ""


def basis_bucket(value, price_scale_tick):
    thresholds = [
        (5.0, "lt_neg_500"),
        (2.0, "neg_500_to_neg_200"),
        (1.0, "neg_200_to_neg_100"),
        (0.5, "neg_100_to_neg_50"),
    ]
    for cents, negative_name in thresholds:
        tick = cents_to_tick(cents, price_scale_tick)
        if value < -tick:
            return negative_name
    if value < cents_to_tick(0.5, price_scale_tick):
        return "neg_50_to_50"
    if value < cents_to_tick(1.0, price_scale_tick):
        return "pos_50_to_100"
    if value < cents_to_tick(2.0, price_scale_tick):
        return "pos_100_to_200"
    if value < cents_to_tick(5.0, price_scale_tick):
        return "pos_200_to_500"
    return "gt_pos_500"


def spread_bucket(value, price_scale_tick):
    thresholds = [
        (0.5, "0_50"),
        (1.0, "50_100"),
        (3.0, "100_300"),
        (7.0, "300_700"),
    ]
    previous_tick = 0
    for cents, name in thresholds:
        tick = cents_to_tick(cents, price_scale_tick)
        if value < tick:
            return name
        previous_tick = tick
    return "gt_700"


def z_value(row):
    spot = to_float(row, "spot")
    barrier = to_float(row, "barrier_price")
    vol = to_float(row, "annualized_vol")
    tte = to_float(row, "tte_years")
    if spot <= 0.0 or barrier <= 0.0 or vol <= 0.0 or tte <= 0.0:
        return None
    denominator = vol * math.sqrt(tte)
    if denominator <= 0.0:
        return None
    return abs(math.log(barrier / spot)) / denominator


def z_bucket(row):
    z = z_value(row)
    if z is None or not math.isfinite(z):
        return "unknown"
    if z < 0.5:
        return "0_0p5"
    if z < 1.0:
        return "0p5_1"
    if z < 1.5:
        return "1_1p5"
    if z < 2.0:
        return "1p5_2"
    return "gt_2"


def load_rows(path):
    with open(path, newline="") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
    for index, row in enumerate(rows):
        row["_row_index"] = index
        row["_ts_ms"] = to_int(row, "ts_ms")
        row["_token_id"] = row.get("token_id", "")
    return rows


def enrich(rows, horizons):
    by_token = defaultdict(list)
    for row in rows:
        by_token[row["_token_id"]].append(row)
    token_times = {}
    for token_id, token_rows in by_token.items():
        token_rows.sort(key=lambda row: row["_ts_ms"])
        token_times[token_id] = [row["_ts_ms"] for row in token_rows]

    enriched = [dict(row) for row in rows]
    by_index = {row["_row_index"]: row for row in enriched}

    for row in rows:
        token_rows = by_token[row["_token_id"]]
        times = token_times[row["_token_id"]]
        out = by_index[row["_row_index"]]
        current_mid = to_int(row, "book_mid_tick")
        current_fair = to_int(row, "external_fair_tick")
        current_basis = to_int(row, "mid_basis_tick")
        for horizon in horizons:
            suffix = f"{horizon}s"
            target_ts = row["_ts_ms"] + horizon * 1000
            future_idx = bisect.bisect_left(times, target_ts)
            if future_idx >= len(token_rows):
                out[f"future_book_mid_tick_{suffix}"] = ""
                out[f"future_external_fair_tick_{suffix}"] = ""
                out[f"future_mid_basis_tick_{suffix}"] = ""
                out[f"market_mid_markout_tick_{suffix}"] = ""
                out[f"external_fair_markout_tick_{suffix}"] = ""
                out[f"basis_change_tick_{suffix}"] = ""
                continue
            future = token_rows[future_idx]
            future_mid = to_int(future, "book_mid_tick")
            future_fair = to_int(future, "external_fair_tick")
            future_basis = to_int(future, "mid_basis_tick")
            out[f"future_book_mid_tick_{suffix}"] = future_mid
            out[f"future_external_fair_tick_{suffix}"] = future_fair
            out[f"future_mid_basis_tick_{suffix}"] = future_basis
            out[f"market_mid_markout_tick_{suffix}"] = future_mid - current_mid
            out[f"external_fair_markout_tick_{suffix}"] = (
                future_fair - current_fair
            )
            out[f"basis_change_tick_{suffix}"] = future_basis - current_basis

    return enriched


def write_csv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fieldnames})


def write_basis_summary(rows, output_dir):
    groups = defaultdict(list)
    for row in rows:
        groups[(row["market_id"], row["token_id"])].append(row)

    output = []
    for (market_id, token_id), group in sorted(groups.items()):
        mid_basis = [to_int(row, "mid_basis_tick") for row in group]
        output.append({
            "market_id": market_id,
            "token_id": token_id,
            "count": len(group),
            "mean_mid_basis_tick": mean(mid_basis),
            "median_mid_basis_tick": median(mid_basis),
            "p10_mid_basis_tick": percentile(mid_basis, 0.10),
            "p25_mid_basis_tick": percentile(mid_basis, 0.25),
            "p75_mid_basis_tick": percentile(mid_basis, 0.75),
            "p90_mid_basis_tick": percentile(mid_basis, 0.90),
            "mean_abs_mid_basis_tick": mean(abs(value) for value in mid_basis),
            "mean_micro_basis_tick": avg(group, "micro_basis_tick"),
            "mean_spread_tick": avg(group, "spread_tick"),
            "mean_buy_edge_tick": avg(group, "buy_edge_tick"),
            "mean_sell_edge_tick": avg(group, "sell_edge_tick"),
        })

    fields = [
        "market_id", "token_id", "count", "mean_mid_basis_tick",
        "median_mid_basis_tick", "p10_mid_basis_tick", "p25_mid_basis_tick",
        "p75_mid_basis_tick", "p90_mid_basis_tick",
        "mean_abs_mid_basis_tick", "mean_micro_basis_tick",
        "mean_spread_tick", "mean_buy_edge_tick", "mean_sell_edge_tick",
    ]
    write_csv(output_dir / "basis_summary.csv", output, fields)


def write_edge_frequency(rows, output_dir, edge_thresholds_cents, price_scale_tick):
    groups = defaultdict(list)
    for row in rows:
        groups[(row["market_id"], row["token_id"])].append(row)

    output = []
    for (market_id, token_id), group in sorted(groups.items()):
        row_out = {
            "market_id": market_id,
            "token_id": token_id,
            "count": len(group),
            "buy_edge_positive_rate": rate(
                group, lambda row: to_int(row, "buy_edge_tick") > 0
            ),
            "sell_edge_positive_rate": rate(
                group, lambda row: to_int(row, "sell_edge_tick") > 0
            ),
        }
        for cents in edge_thresholds_cents:
            tick = cents_to_tick(cents, price_scale_tick)
            row_out[edge_frequency_column("buy", cents)] = rate(
                group, lambda row, tick=tick: to_int(row, "buy_edge_tick") >= tick
            )
            row_out[edge_frequency_column("sell", cents)] = rate(
                group, lambda row, tick=tick: to_int(row, "sell_edge_tick") >= tick
            )
        output.append(row_out)

    fields = [
        "market_id", "token_id", "count", "buy_edge_positive_rate",
        "sell_edge_positive_rate",
    ]
    for cents in edge_thresholds_cents:
        fields.append(edge_frequency_column("buy", cents))
        fields.append(edge_frequency_column("sell", cents))
    write_csv(output_dir / "edge_frequency.csv", output, fields)


def write_bucket_markout(rows, output_dir, horizons, bucket_name, bucket_fn, filename):
    groups = defaultdict(list)
    for row in rows:
        groups[bucket_fn(row)].append(row)

    output = []
    for bucket, group in sorted(groups.items()):
        for horizon in horizons:
            suffix = f"{horizon}s"
            valid = [
                row for row in group
                if row.get(f"market_mid_markout_tick_{suffix}", "") != ""
            ]
            output.append({
                bucket_name: bucket,
                "horizon_seconds": horizon,
                "count": len(valid),
                "avg_market_mid_markout_tick": avg(
                    valid, f"market_mid_markout_tick_{suffix}"
                ),
                "avg_external_fair_markout_tick": avg(
                    valid, f"external_fair_markout_tick_{suffix}"
                ),
                "avg_basis_change_tick": avg(
                    valid, f"basis_change_tick_{suffix}"
                ),
            })

    fields = [
        bucket_name, "horizon_seconds", "count",
        "avg_market_mid_markout_tick", "avg_external_fair_markout_tick",
        "avg_basis_change_tick",
    ]
    write_csv(output_dir / filename, output, fields)


def signal_predicates(edge_thresholds_cents, price_scale_tick):
    predicates = {}
    for cents in edge_thresholds_cents:
        tick = cents_to_tick(cents, price_scale_tick)
        predicates[edge_signal_name("buy", cents)] = (
            lambda row, tick=tick: to_int(row, "buy_edge_tick") >= tick
        )
        predicates[edge_signal_name("sell", cents)] = (
            lambda row, tick=tick: to_int(row, "sell_edge_tick") >= tick
        )
    return predicates


def trade_filter(row, max_spread_tick):
    return (
        to_int(row, "spread_tick") <= max_spread_tick and
        to_int(row, "bid_size") + to_int(row, "ask_size") > 0 and
        to_int(row, "spot_age_ms") <= 1500 and
        to_int(row, "vol_age_ms") <= 60000
    )


def write_trade_signal_report(
    rows,
    output_dir,
    horizons,
    edge_thresholds_cents,
    price_scale_tick,
    max_spread_tick,
):
    output = []
    for signal, predicate in signal_predicates(
        edge_thresholds_cents,
        price_scale_tick,
    ).items():
        side = "buy" if signal.startswith("buy_") else "sell"
        signal_rows = [
            row for row in rows
            if trade_filter(row, max_spread_tick) and predicate(row)
        ]
        for horizon in horizons:
            suffix = f"{horizon}s"
            valid = [
                row for row in signal_rows
                if row.get(f"future_book_mid_tick_{suffix}", "") != ""
            ]
            market_pnls = []
            fair_pnls = []
            entry_edges = []
            for row in valid:
                future_mid = to_int(row, f"future_book_mid_tick_{suffix}")
                future_fair = to_int(row, f"future_external_fair_tick_{suffix}")
                if side == "buy":
                    entry = to_int(row, "best_ask_tick")
                    entry_edges.append(to_int(row, "buy_edge_tick"))
                    market_pnls.append(future_mid - entry)
                    fair_pnls.append(future_fair - entry)
                else:
                    entry = to_int(row, "best_bid_tick")
                    entry_edges.append(to_int(row, "sell_edge_tick"))
                    market_pnls.append(entry - future_mid)
                    fair_pnls.append(entry - future_fair)

            output.append({
                "signal": signal,
                "horizon_seconds": horizon,
                "trades": len(valid),
                "avg_entry_edge_tick": mean(entry_edges) if entry_edges else "",
                "avg_market_markout_pnl_tick": mean(market_pnls) if market_pnls else "",
                "avg_fair_markout_pnl_tick": mean(fair_pnls) if fair_pnls else "",
                "market_win_rate": (
                    sum(1 for value in market_pnls if value > 0) / len(market_pnls)
                    if market_pnls else ""
                ),
                "fair_win_rate": (
                    sum(1 for value in fair_pnls if value > 0) / len(fair_pnls)
                    if fair_pnls else ""
                ),
            })

    fields = [
        "signal", "horizon_seconds", "trades", "avg_entry_edge_tick",
        "avg_market_markout_pnl_tick", "avg_fair_markout_pnl_tick",
        "market_win_rate", "fair_win_rate",
    ]
    write_csv(output_dir / "trade_signal_report.csv", output, fields)


def parse_horizons(value):
    if not value:
        return DEFAULT_HORIZONS
    return [int(part.strip()) for part in value.split(",") if part.strip()]


def parse_edge_thresholds_cents(value):
    if not value:
        return list(DEFAULT_EDGE_THRESHOLD_CENTS)
    return [float(part.strip()) for part in value.split(",") if part.strip()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument(
        "--markout-horizons-seconds",
        default=",".join(str(horizon) for horizon in DEFAULT_HORIZONS),
    )
    parser.add_argument(
        "--price-scale-tick",
        type=int,
        default=DEFAULT_PRICE_SCALE_TICK,
    )
    parser.add_argument(
        "--max-spread-cents",
        type=float,
        default=DEFAULT_MAX_SPREAD_CENTS,
    )
    parser.add_argument(
        "--edge-threshold-cents",
        default=",".join(str(cents) for cents in DEFAULT_EDGE_THRESHOLD_CENTS),
    )
    args = parser.parse_args()

    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    horizons = parse_horizons(args.markout_horizons_seconds)
    price_scale_tick = args.price_scale_tick
    max_spread_tick = cents_to_tick(args.max_spread_cents, price_scale_tick)
    edge_thresholds_cents = parse_edge_thresholds_cents(args.edge_threshold_cents)

    rows = load_rows(input_path)
    if not rows:
        raise SystemExit(f"no rows in {input_path}")

    enriched = enrich(rows, horizons)
    base_fields = [field for field in rows[0].keys() if not field.startswith("_")]
    markout_fields = []
    for horizon in horizons:
        suffix = f"{horizon}s"
        markout_fields.extend([
            f"future_book_mid_tick_{suffix}",
            f"future_external_fair_tick_{suffix}",
            f"future_mid_basis_tick_{suffix}",
            f"market_mid_markout_tick_{suffix}",
            f"external_fair_markout_tick_{suffix}",
            f"basis_change_tick_{suffix}",
        ])
    write_csv(
        output_dir / "external_fair_basis_enriched.csv",
        enriched,
        base_fields + markout_fields,
    )

    write_basis_summary(enriched, output_dir)
    write_edge_frequency(
        enriched,
        output_dir,
        edge_thresholds_cents,
        price_scale_tick,
    )
    write_bucket_markout(
        enriched,
        output_dir,
        horizons,
        "basis_bucket",
        lambda row: basis_bucket(
            to_int(row, "mid_basis_tick"),
            price_scale_tick,
        ),
        "markout_by_basis_bucket.csv",
    )
    write_bucket_markout(
        enriched,
        output_dir,
        horizons,
        "spread_bucket",
        lambda row: spread_bucket(
            to_int(row, "spread_tick"),
            price_scale_tick,
        ),
        "markout_by_spread_bucket.csv",
    )
    write_bucket_markout(
        enriched,
        output_dir,
        horizons,
        "z_bucket",
        z_bucket,
        "markout_by_z_bucket.csv",
    )
    write_trade_signal_report(
        enriched,
        output_dir,
        horizons,
        edge_thresholds_cents,
        price_scale_tick,
        max_spread_tick,
    )


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import mean, median


DEFAULT_PRICE_SCALE_TICK = 1_000_000


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
    pos = (len(ordered) - 1) * p
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    weight = pos - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight


def write_csv(path, rows, fields):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def summarize(rows, price_scale_tick):
    ticks_per_cent = price_scale_tick / 100.0
    groups = defaultdict(list)
    for row in rows:
        groups[(row.get("market_id", ""), row.get("token_id", ""))].append(row)

    out = []
    for (market_id, token_id), group in sorted(groups.items()):
        basis_raw = [to_int(row, "basis_raw") for row in group]
        basis_tradable = [to_int(row, "basis_tradable") for row in group]
        buy_edge = [to_int(row, "buy_edge") for row in group]
        sell_edge = [to_int(row, "sell_edge") for row in group]
        spot_age = [to_int(row, "spot_age_ms") for row in group]
        risk_rejects = [
            row for row in group
            if row.get("risk_decision", "") not in ("approve", "no_quote")
        ]
        out.append({
            "market_id": market_id,
            "token_id": token_id,
            "rows": len(group),
            "quote_rows": sum(1 for row in group if row.get("quote_side") != "none"),
            "no_quote_rows": sum(1 for row in group if row.get("risk_decision") == "no_quote"),
            "risk_reject_rows": len(risk_rejects),
            "mean_canonical_raw_basis_cents": mean(basis_raw) / ticks_per_cent,
            "median_canonical_raw_basis_cents": median(basis_raw) / ticks_per_cent,
            "p10_canonical_raw_basis_cents": percentile(basis_raw, 0.10) / ticks_per_cent,
            "p90_canonical_raw_basis_cents": percentile(basis_raw, 0.90) / ticks_per_cent,
            "mean_canonical_tradable_basis_cents": mean(basis_tradable) / ticks_per_cent,
            "mean_asset_buy_edge_cents": mean(buy_edge) / ticks_per_cent,
            "mean_asset_sell_edge_cents": mean(sell_edge) / ticks_per_cent,
            "max_spot_age_ms": max(spot_age) if spot_age else "",
        })
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument(
        "--price-scale-tick",
        type=int,
        default=DEFAULT_PRICE_SCALE_TICK,
    )
    args = parser.parse_args()

    with open(args.input, newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit(f"no rows in {args.input}")

    fields = [
        "market_id",
        "token_id",
        "rows",
        "quote_rows",
        "no_quote_rows",
        "risk_reject_rows",
        "mean_canonical_raw_basis_cents",
        "median_canonical_raw_basis_cents",
        "p10_canonical_raw_basis_cents",
        "p90_canonical_raw_basis_cents",
        "mean_canonical_tradable_basis_cents",
        "mean_asset_buy_edge_cents",
        "mean_asset_sell_edge_cents",
        "max_spot_age_ms",
    ]
    write_csv(
        Path(args.output_dir) / "canonical_quote_summary.csv",
        summarize(rows, args.price_scale_tick),
        fields,
    )


if __name__ == "__main__":
    main()

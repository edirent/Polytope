#!/usr/bin/env python3
"""Finalize a partial BTC 5m full-market capture.

It keeps only markets whose samples reached settlement time, fetches settlements,
and writes enriched CSV/settlement JSON suitable for trade-rule audits.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import math
from pathlib import Path
from typing import Any

from btc5m_full_market_capture import (
    MarketInfo,
    csv_fields,
    enrich_rows,
    fetch_settlements,
    fmt,
    market_capture_summary,
    sanitize_json,
    source_coverage,
)


def number(value: Any, default: float = math.nan) -> float:
    if value is None or value == "":
        return default
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    return out if math.isfinite(out) else default


def load_rows(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="", encoding="utf-8") as fh:
        return [dict(row) for row in csv.DictReader(fh)]


def completed_market_slugs(rows: list[dict[str, Any]], max_min_tau_sec: float) -> set[str]:
    by_market: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_market.setdefault(str(row["market_slug"]), []).append(row)
    out = set()
    for slug, market_rows in by_market.items():
        min_tau = min(number(row.get("tau_sec")) for row in market_rows)
        if min_tau <= max_min_tau_sec:
            out.add(slug)
    return out


def market_infos(rows: list[dict[str, Any]]) -> list[MarketInfo]:
    by_market: dict[str, dict[str, Any]] = {}
    for row in rows:
        by_market.setdefault(str(row["market_slug"]), row)
    infos = []
    for row in by_market.values():
        infos.append(MarketInfo(
            slug=str(row["market_slug"]),
            title=str(row["market_slug"]),
            condition_id=str(row.get("condition_id", "")),
            up_token=str(row.get("up_token", "")),
            down_token=str(row.get("down_token", "")),
            start_ts=int(number(row.get("round_start_ts"))),
            end_ts=int(number(row.get("round_end_ts"))),
        ))
    infos.sort(key=lambda info: info.start_ts)
    return infos


def write_enriched(path: Path, rows: list[dict[str, Any]], qtys: list[float]) -> None:
    fields = csv_fields(qtys, enriched=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: fmt(row.get(key)) for key in fields})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--max-min-tau-sec", type=float, default=2.0)
    parser.add_argument("--settlement-timeout-sec", type=int, default=900)
    parser.add_argument("--settlement-poll-sec", type=int, default=15)
    args = parser.parse_args()

    run_dir = Path(args.run_dir)
    rows_all = load_rows(run_dir / "samples.csv")
    completed = completed_market_slugs(rows_all, args.max_min_tau_sec)
    rows = [
        row for row in rows_all
        if str(row["market_slug"]) in completed
    ]
    markets = market_infos(rows)
    settlements = fetch_settlements(
        markets,
        rows,
        args.settlement_timeout_sec,
        args.settlement_poll_sec,
    )
    enriched = enrich_rows(rows, [5.0, 25.0, 50.0], settlements)

    enriched_path = run_dir / "samples_enriched_partial_complete_markets.csv"
    settlements_path = run_dir / "settlements_partial_complete_markets.json"
    metadata_path = run_dir / "partial_complete_markets_metadata.json"
    write_enriched(enriched_path, enriched, [5.0, 25.0, 50.0])
    settlements_path.write_text(
        json.dumps(sanitize_json(settlements), indent=2),
        encoding="utf-8",
    )
    metadata = {
        "run_dir": str(run_dir),
        "source_samples": str(run_dir / "samples.csv"),
        "enriched_csv": str(enriched_path),
        "settlements": str(settlements_path),
        "completed_market_count": len(markets),
        "row_count": len(enriched),
        "excluded_incomplete_markets": sorted(
            {str(row["market_slug"]) for row in rows_all} - completed
        ),
        "markets": [dataclasses.asdict(market) for market in markets],
        "market_capture_summary": market_capture_summary(enriched, markets),
        "source_coverage": source_coverage(enriched),
    }
    metadata_path.write_text(
        json.dumps(sanitize_json(metadata), indent=2),
        encoding="utf-8",
    )
    print(enriched_path)
    print(settlements_path)
    print(metadata_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

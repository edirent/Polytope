"""Independent, read-only audit of native V0.1 reports and saved inputs.

Uses Decimal and hashlib from Python's standard library. This verifies local
arithmetic/integrity, not exchange provenance, fills, or realized profits.
"""
import argparse
import csv
import hashlib
import json
from collections import defaultdict
from decimal import Decimal
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def checked_path(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError("unsafe audit path")
    return path


def audit(run, analysis):
    summary = json.loads((analysis / "summary.json").read_text())
    manifest = json.loads((analysis / "audit_manifest.json").read_text())
    if summary["verified_live_realized_pnl"] is not None or summary["live_orders_sent"] != 0:
        raise ValueError("V0.1 must have null live realized PnL and zero live orders")
    hashes = 0
    for section, root in (("deterministic_inputs", run), ("deterministic_outputs", analysis), ("raw_inventory", run)):
        for entry in manifest[section]:
            if digest(checked_path(root, entry["file"])) != entry["sha256"]:
                raise ValueError(f"hash mismatch: {section}/{entry['file']}")
            hashes += 1
    source_manifest = json.loads((analysis / "analysis_source_manifest.json").read_text())
    for entry in source_manifest["files"]:
        if digest(checked_path(analysis / "analysis_source_snapshot", entry["file"])) != entry["sha256"]:
            raise ValueError(f"analysis source mismatch: {entry['file']}")
        hashes += 1
    source_records = []
    if (run / "metadata_requests.jsonl").exists():
        source_records.extend(json.loads(line) for line in (run / "metadata_requests.jsonl").read_text().splitlines())
    if (run / "fee_source_audit.json").exists():
        source_records.extend(json.loads((run / "fee_source_audit.json").read_text()))
    for entry in source_records:
        if "raw_file" in entry and "sha256" in entry:
            if digest(checked_path(run, entry["raw_file"])) != entry["sha256"]:
                raise ValueError(f"metadata/fee response mismatch: {entry['raw_file']}")
            hashes += 1
    totals = defaultdict(lambda: {"trades": 0, "net": Decimal(0), "fees": Decimal(0)})
    for row in rows(analysis / "pnl_by_trade.csv"):
        if row["verified_live_realized_pnl"]:
            raise ValueError("CSV contains claimed live PnL")
        if row["lifecycle"] != "SIMULATED":
            continue
        floor = min(Decimal(row["qty_yes"]), Decimal(row["qty_no"])) - Decimal(row["actual_simulated_cost"])
        if floor != Decimal(row["hypothetical_contractual_floor_pnl"]):
            raise ValueError("floor PnL omits cost or residual exposure")
        if floor != Decimal(row["partial_hedge_worst_case_pnl"]):
            raise ValueError("partial-hedge floor mismatch")
        group = totals[(int(row["latency_ms"]), row["execution_model"])]
        group["trades"] += 1
        group["net"] += floor
        group["fees"] += Decimal(row["fees"])
    for row in summary["latency_scenarios"]:
        group = totals[(row["latency_ms"], row["execution_model"])]
        if row["simulated_trades"] != group["trades"]:
            raise ValueError("trade count mismatch")
        if row["simulated_net_pnl"] is not None:
            if Decimal(row["simulated_net_pnl"]) != group["net"] or Decimal(row["fees"]) != group["fees"]:
                raise ValueError("scenario totals mismatch")
            if Decimal(row["simulated_gross_pnl"]) - Decimal(row["fees"]) - Decimal(row["modeled_execution_loss"]) != group["net"]:
                raise ValueError("fees/execution losses do not reconcile to net")
            committed = Decimal(row["max_capital_committed"])
            if committed and abs(Decimal(str(row["return_on_committed_capital"])) - group["net"] / committed) > Decimal("0.000000000001"):
                raise ValueError("return denominator omits reserved capital")
        elif group["trades"]:
            raise ValueError("simulated trades hidden behind unmeasured totals")
    for row in rows(analysis / "capital_utilization.csv"):
        reserved, ceiling = Decimal(row["reserved_cash"]), Decimal(row["ceiling"])
        k, p = Decimal(row["kalshi_cash_available"]), Decimal(row["polymarket_cash_available"])
        if not (0 <= reserved <= ceiling and k >= 0 and p >= 0 and k + p + reserved == ceiling):
            raise ValueError("venue wallets reused or portfolio ceiling exceeded")
    return {"status": "LOCAL_AUDIT_PASSED", "hashes_verified": hashes,
            "simulated_trades_recomputed": sum(g["trades"] for g in totals.values()),
            "data_evidence": summary["evidence_level"], "economic_verdict": summary["verdict"],
            "verified_live_realized_pnl": None,
            "scope": "Local integrity and arithmetic only; no exchange-authenticated execution evidence"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--analysis-dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.run_dir, args.analysis_dir or args.run_dir / "analysis"), indent=2))

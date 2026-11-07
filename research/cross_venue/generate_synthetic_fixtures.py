"""Reproducibly generate synthetic tests; never imports or fabricates live data."""
import copy
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "src"))
from reference import UNIT, decimal, pair_hash, sha


def packet(key, bids, asks, recv=900_000_000, sequence=1, kind="snapshot", offset=0):
    raw = json.dumps({"bids": bids, "asks": asks, "complete": True}, sort_keys=True, separators=(",", ":"))
    return {"key": key, "kind": kind, "recv_monotonic_ns": recv, "source_ts_ns": recv - 1_000_000,
            "sequence": sequence, "raw_log_offset": offset, "raw_payload": raw, "raw_hash": sha(raw)}


def main():
    # SYNTHETIC_USD is an artificial common payout unit, NOT USD/USDC parity.
    a = {"venue": "kalshi", "market_id": "SYNTHETIC-K", "instrument_id": "YES", "currency": "SYNTHETIC_USD"}
    b = {"venue": "polymarket_international", "market_id": "SYNTHETIC-P", "instrument_id": "NO", "currency": "SYNTHETIC_USD"}
    pair = {"pair_id": "SYNTHETIC-K-P", "leg_yes": a, "leg_no": b, "rules_hash_yes": sha("synthetic-k-rules"),
            "rules_hash_no": sha("synthetic-p-rules"), "review_id": "SYNTHETIC-REVIEW-NOT-HUMAN-LIVE-APPROVAL",
            "approval_revision": 1, "enabled": True, "synthetic": True,
            "payoffs": [{"name": "YES", "yes": "1", "no": "0"}, {"name": "NO", "yes": "0", "no": "1"}]}
    pair["artifact_hash"] = pair_hash(pair)
    events = [packet(a, [["0.45", 100]], [["0.47", 100]]), packet(b, [["0.46", 100]], [["0.48", 100]], 950_000_000, offset=1)]
    fee_quotes = [{"qty": q, "fees": [decimal(17_500 * q), decimal(17_400 * q)], "slippage": ["0", "0"],
                   "version": "SYNTHETIC-BUDGET-v1", "pair_artifact_hash": pair["artifact_hash"],
                   "valid_from_ns": 1, "valid_until_ns": 2_000_000_000} for q in (25, 50, 100, 200, 500)]
    scenarios = [{"name": "matched_100", "now_ns": 1_000_000_000, "events": events}]
    walked = copy.deepcopy(events)
    walked[0] = packet(a, [["0.45", 100]], [["0.47", 40], ["0.49", 60]])
    scenarios.append({"name": "depth_walk", "now_ns": 1_000_000_000, "events": walked})
    scenarios.append({"name": "stale", "now_ns": 1_500_000_000, "events": events})
    skew = copy.deepcopy(events)
    skew[0]["recv_monotonic_ns"] = 800_000_000
    scenarios.append({"name": "receive_skew", "now_ns": 1_000_000_000, "events": skew})
    gap = events + [packet(a, [], [["0.46", 100]], 970_000_000, 3, "delta", 2)]
    scenarios.append({"name": "sequence_gap", "now_ns": 1_000_000_000, "events": gap})
    disconnected = events + [packet(a, [], [], 970_000_000, 2, "disconnect", 2)]
    scenarios.append({"name": "disconnect", "now_ns": 1_000_000_000, "events": disconnected})
    recovered = gap + [packet(a, [["0.45", 100]], [["0.47", 100]], 980_000_000, 10, "snapshot", 3)]
    scenarios.append({"name": "resnapshot", "now_ns": 1_000_000_000, "events": recovered})
    books = {"schema": "cross-venue-books-v0", "mode": "SYNTHETIC", "pair": pair, "starting_cash": ["1000", "1000"],
             "fee_quotes": fee_quotes, "scenarios": scenarios}

    def fill(name, key, payoff, quantity, price, fee, offset):
        receipt = {"key": key, "fill_id": f"{name}-{payoff}", "order_id": f"{name}-order-{payoff}", "pair_id": pair["pair_id"],
                   "canonical_payoff": payoff, "qty": quantity, "price": price, "fee_paid": fee,
                   "exchange_time_ns": 1_100_000_000, "received_time_ns": 1_150_000_000, "raw_log_offset": offset,
                   "fee_version": "SYNTHETIC-RECEIPT-v1", "mode": "SIMULATED"}
        raw = json.dumps(receipt, sort_keys=True, separators=(",", ":"))
        return {**receipt, "raw_payload": raw, "raw_hash": sha(raw)}

    cases = []
    for name, qno, price, fee_no in (("matched_100", 100, "0.48", "1.74"), ("partial_hedge_60", 60, "0.48", "1.05"),
                                     ("hedge_slippage", 100, "0.52", "1.74"), ("signal_no_fills", 0, "0.48", "0")):
        receipts = [] if qno == 0 else [fill(name, a, "YES", 100, "0.47", "1.75", 0), fill(name, b, "NO", qno, price, fee_no, 1)]
        cases.append({"name": name, "planned_quantity": 100, "cutoff_ns": 2_000_000_000, "illustrative_outcome": "YES", "fills": receipts})
    fills = {"schema": "cross-venue-fills-v0", "mode": "SYNTHETIC", "pair": pair, "starting_cash": ["1000", "1000"], "scenarios": cases}
    fixture_dir = ROOT / "fixtures"
    fixture_dir.mkdir(parents=True, exist_ok=True)
    for name, data in (("synthetic_books.json", books), ("synthetic_fills.json", fills), ("approved_pair.synthetic.json", pair)):
        (fixture_dir / name).write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

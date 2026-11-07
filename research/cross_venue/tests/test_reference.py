import copy
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from reference import (BookStore, UNIT, audit_scenario, decimal, load, money, pair_hash, quote, scan, sha, validate_pair)


class ReferenceTest(unittest.TestCase):
    def setUp(self):
        self.books = load(ROOT / "fixtures/synthetic_books.json", "cross-venue-books-v0")
        self.fills = load(ROOT / "fixtures/synthetic_fills.json", "cross-venue-fills-v0")

    def store(self, index=0):
        store = BookStore()
        for event in self.books["scenarios"][index]["events"]:
            store.apply(event)
        return store

    def rebuild_receipt_raw(self, fill):
        fill["raw_payload"] = json.dumps({k: v for k, v in fill.items() if k not in ("raw_hash", "raw_payload")}, sort_keys=True, separators=(",", ":"))
        fill["raw_hash"] = sha(fill["raw_payload"])

    def test_decimal_is_exact(self):
        self.assertEqual(money("0.10") + money("0.20"), money("0.30"))
        self.assertEqual(decimal(money("-18.60")), "-18.600000")
        self.assertEqual(money(decimal(-2**63)), -2**63)
        for value in ("1e-2", "NaN", "0.0000001", ".1", "1.", "+1", "9223372036854.775808", 0.47):
            with self.subTest(value=value), self.assertRaises(ValueError):
                money(value)

    def test_accounting_golden_cases(self):
        expected = {"matched_100": "1.510000", "partial_hedge_60": "-18.600000", "hedge_slippage": "-2.490000", "signal_no_fills": "0.000000"}
        for case in self.fills["scenarios"]:
            result = audit_scenario(self.fills["pair"], case, self.fills["starting_cash"])
            self.assertEqual(result["filled_locked_floor_usd"], expected[case["name"]])
            self.assertEqual(result["verified_live_realized_pnl_usd"], "")

    def test_depth_walk_changes_edge(self):
        fee = self.books["fee_quotes"][2]
        first = quote(self.books["pair"], self.store(), fee, 1_000_000_000)
        walked = quote(self.books["pair"], self.store(1), fee, 1_000_000_000)
        self.assertEqual(first["quoted_edge_usd"], "1.510000")
        self.assertEqual(walked["quoted_edge_usd"], "0.310000")
        self.assertEqual(walked["risk_eligible"], "false")

    def test_stale_skew_gap_disconnect_never_emit(self):
        names = {r["scenario"] for r in scan(self.books)}
        self.assertEqual(names, {"matched_100", "depth_walk", "resnapshot"})

    def test_resnapshot_recovers_and_changes_id(self):
        fee = self.books["fee_quotes"][2]
        first = quote(self.books["pair"], self.store(), fee, 1_000_000_000)
        recovered = quote(self.books["pair"], self.store(6), fee, 1_000_000_000)
        self.assertNotEqual(first["candidate_id"], recovered["candidate_id"])

    def test_missing_rule_review_or_independent_void_rejected(self):
        for change in ("disabled", "unreviewed", "void", "fx"):
            pair = copy.deepcopy(self.books["pair"])
            if change == "disabled": pair["enabled"] = False
            if change == "unreviewed": pair["review_id"] = ""
            if change == "void": pair["payoffs"].append({"name": "VOID", "yes": "0", "no": "0"})
            if change == "fx": pair["leg_no"]["currency"] = "USDC"
            pair["artifact_hash"] = pair_hash(pair)
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate_pair(pair)

    def test_artifact_mutation(self):
        pair = copy.deepcopy(self.books["pair"])
        pair["approval_revision"] += 1
        with self.assertRaises(ValueError): validate_pair(pair)

    def test_negative_or_expired_fee(self):
        for change in ("negative", "expired", "scope"):
            fee = copy.deepcopy(self.books["fee_quotes"][2])
            if change == "negative": fee["fees"][0] = "-1"
            if change == "expired": fee["valid_until_ns"] = 1_000_000_000
            if change == "scope": fee["pair_artifact_hash"] = "0" * 64
            with self.subTest(change=change), self.assertRaises(ValueError):
                quote(self.books["pair"], self.store(), fee, 1_000_000_000)

    def test_insufficient_depth_not_extrapolated(self):
        with self.assertRaises(ValueError):
            quote(self.books["pair"], self.store(), self.books["fee_quotes"][3], 1_000_000_000)

    def test_duplicate_fill_not_double_counted(self):
        case = copy.deepcopy(self.fills["scenarios"][0])
        case["fills"].append(copy.deepcopy(case["fills"][0]))
        result = audit_scenario(self.fills["pair"], case, self.fills["starting_cash"])
        self.assertEqual(result["qty_yes"], 100)
        self.assertEqual(result["filled_locked_floor_usd"], "1.510000")

    def test_conflicting_fill_rejected(self):
        case = copy.deepcopy(self.fills["scenarios"][0])
        conflict = copy.deepcopy(case["fills"][0])
        conflict["qty"] = 99
        self.rebuild_receipt_raw(conflict)
        case["fills"].append(conflict)
        with self.assertRaises(ValueError):
            audit_scenario(self.fills["pair"], case, self.fills["starting_cash"])

    def test_invalid_receipt_fee_time_or_mode(self):
        for change in ("negative_fee", "reversed_time", "live_mode", "cutoff", "wrong_key", "invalid_payoff"):
            case = copy.deepcopy(self.fills["scenarios"][0])
            fill = case["fills"][0]
            if change == "negative_fee": fill["fee_paid"] = "-1"
            if change == "reversed_time": fill["received_time_ns"] = fill["exchange_time_ns"] - 1
            if change == "live_mode": fill["mode"] = "LIVE"
            if change == "cutoff": fill["received_time_ns"] = case["cutoff_ns"] + 1
            if change == "wrong_key": fill["key"]["venue"] = "polymarket_us"
            if change == "invalid_payoff": fill["canonical_payoff"] = "UNKNOWN"
            self.rebuild_receipt_raw(fill)
            with self.subTest(change=change), self.assertRaises(ValueError):
                audit_scenario(self.fills["pair"], case, self.fills["starting_cash"])

    def test_raw_envelope_cannot_disagree(self):
        case = copy.deepcopy(self.fills["scenarios"][0])
        case["fills"][0]["price"] = "0.01"
        with self.assertRaises(ValueError): audit_scenario(self.fills["pair"], case, self.fills["starting_cash"])

    def test_incomplete_empty_crossed_and_duplicate_book(self):
        for change in ("incomplete", "empty", "crossed", "duplicate"):
            event = copy.deepcopy(self.books["scenarios"][0]["events"][0])
            raw = json.loads(event["raw_payload"])
            if change == "incomplete": raw["complete"] = False
            if change == "empty": raw["asks"] = []
            if change == "crossed": raw["bids"][0][0] = "0.60"
            if change == "duplicate": raw["asks"].append(raw["asks"][0])
            event["raw_payload"] = json.dumps(raw)
            event["raw_hash"] = sha(event["raw_payload"])
            with self.subTest(change=change): self.assertFalse(BookStore().apply(event))

    def test_hash_is_local_integrity_only(self):
        changed = copy.deepcopy(self.fills)
        original = sha(json.dumps(changed))
        changed["scenarios"][0]["fills"][0]["fee_paid"] = "0"
        self.assertNotEqual(sha(json.dumps(changed)), original)


if __name__ == "__main__":
    unittest.main()

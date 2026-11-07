"""Dependency-free, offline reference math. Never sends orders or attests live PnL."""
from __future__ import annotations

import csv
import hashlib
import json
import re
from pathlib import Path

UNIT = 1_000_000
MAX_QTY = 1_000_000_000
LIMIT = 2**63 - 1


def require(value, reason):
    if not value:
        raise ValueError(reason)


def checked(value):
    require(-2**63 <= value <= LIMIT, "money overflow")
    return value


def money(value):
    require(isinstance(value, str) and len(value) <= 24 and re.fullmatch(r"-?[0-9]+(?:\.[0-9]{1,6})?", value), "invalid decimal")
    negative = value.startswith("-")
    whole, _, fraction = value.lstrip("-").partition(".")
    result = int(whole) * UNIT + int(fraction.ljust(6, "0") or "0")
    return checked(-result if negative else result)


def decimal(value):
    return f"{'-' if value < 0 else ''}{abs(value) // UNIT}.{abs(value) % UNIT:06d}"


def uint(value):
    require(type(value) is int and 0 <= value < 2**64, "invalid unsigned integer")
    return value


def qty(value):
    require(uint(value) <= MAX_QTY, "quantity too large")
    return value


def sha(value):
    return hashlib.sha256(value.encode() if isinstance(value, str) else value).hexdigest()


def field(value):
    value = str(value)
    return f"{len(value.encode())}:{value}"


def key_id(key):
    require(key["venue"] in ("kalshi", "polymarket_international", "polymarket_us"), "unknown venue")
    return "".join(field(key[name]) for name in ("venue", "market_id", "instrument_id", "currency"))


def pair_hash(pair):
    values = ["cross-venue-pair-v0", pair["pair_id"], key_id(pair["leg_yes"]), key_id(pair["leg_no"]),
              pair["rules_hash_yes"], pair["rules_hash_no"], pair["review_id"], pair["approval_revision"],
              "1" if pair["enabled"] else "0", "1" if pair["synthetic"] else "0"]
    for state in pair["payoffs"]:
        values.extend((state["name"], money(state["yes"]), money(state["no"])))
    return sha("".join(field(v) for v in values))


def validate_pair(pair):
    require(pair["enabled"] is True and pair["synthetic"] is True, "not an enabled synthetic approval")
    require(pair["pair_id"] and pair["review_id"] and uint(pair["approval_revision"]) > 0, "missing approval")
    a, b = pair["leg_yes"], pair["leg_no"]
    require(a["venue"] != b["venue"] and a["currency"] == b["currency"] and a["currency"], "venue/currency mismatch")
    for key in (a, b):
        key_id(key)
        require(key["market_id"] and key["instrument_id"], "missing instrument")
    for name in ("rules_hash_yes", "rules_hash_no"):
        require(re.fullmatch(r"[0-9a-f]{64}", pair[name]), "missing rule hash")
    names, payouts = set(), set()
    for state in pair["payoffs"]:
        require(state["name"] and state["name"] not in names, "duplicate payoff state")
        names.add(state["name"])
        payoff = (money(state["yes"]), money(state["no"]))
        require(payoff in ((UNIT, 0), (0, UNIT)), "non-complementary payoff")
        payouts.add(payoff)
    require(len(payouts) == 2 and pair["artifact_hash"] == pair_hash(pair), "artifact/coverage mismatch")


def load(path, schema):
    data = json.loads(Path(path).read_bytes())
    require(data["schema"] == schema and data["mode"] == "SYNTHETIC", "only SYNTHETIC V0 fixtures accepted")
    validate_pair(data["pair"])
    require(len(data["starting_cash"]) == 2, "two accounts required")
    require(all(money(v) >= 0 for v in data["starting_cash"]), "negative starting cash")
    names = [s["name"] for s in data["scenarios"]]
    require(len(names) == len(set(names)), "duplicate scenario")
    return data


class BookStore:
    def __init__(self):
        self.books = {}

    def apply(self, event):
        identity = key_id(event["key"])
        old = self.books.setdefault(identity, {"valid": False, "version": 0, "recv": 0})
        raw = event["raw_payload"]
        require(sha(raw) == event["raw_hash"], "raw checksum mismatch")
        payload = json.loads(raw)
        recv = uint(event["recv_monotonic_ns"])
        kind = event["kind"]
        require(kind in ("snapshot", "delta", "disconnect", "halt"), "unsupported replay kind")
        seq = uint(event["sequence"])
        uint(event["source_ts_ns"])
        uint(event["raw_log_offset"])
        last_recv = old.get("last_recv", old["recv"])
        old["last_recv"] = max(last_recv, recv)
        if recv == 0 or recv < last_recv or kind in ("disconnect", "halt"):
            old["valid"] = False
            if kind in ("disconnect", "halt"):
                old["recv"] = recv
            return False
        if kind == "delta" and (not old["valid"] or seq != old.get("seq", 0) + 1):
            old["valid"] = False
            return False
        bids, asks = ({}, {}) if kind == "snapshot" else (old["bids"].copy(), old["asks"].copy())
        valid = seq > 0 and payload["complete"] is True
        for name, side in (("bids", bids), ("asks", asks)):
            seen = set()
            for price, quantity in payload[name]:
                price, quantity = money(price), qty(quantity)
                if not 0 < price < UNIT or price in seen:
                    valid = False
                seen.add(price)
                if quantity:
                    side[price] = quantity
                else:
                    side.pop(price, None)
            valid &= len(side) <= 64
        if not valid or not bids or not asks or max(bids) >= min(asks):
            old["valid"] = False
            return False
        version = old["version"] + 1
        state_bytes = field(identity) + field(old.get("depth_hash", 0)) + field(event["raw_hash"])
        for name, side in (("B", bids), ("A", asks)):
            for price, quantity in sorted(side.items()):
                state_bytes += field(name) + field(price) + field(quantity)
        self.books[identity] = {"valid": True, "version": version, "recv": recv, "last_recv": recv, "seq": seq,
                                "event": event, "bids": bids, "asks": asks,
                                "depth_hash": int(sha(state_bytes)[:16], 16)}
        return True


def quote(pair, store, fee, now):
    validate_pair(pair)
    uint(now)
    books = [store.books.get(key_id(pair[name])) for name in ("leg_yes", "leg_no")]
    require(all(b and b["valid"] for b in books), "invalid book quality")
    require(all(0 <= now - b["recv"] <= 250_000_000 for b in books), "stale/future book")
    require(abs(books[0]["recv"] - books[1]["recv"]) <= 100_000_000, "receive skew")
    quantity = qty(fee["qty"])
    require(quantity in (25, 50, 100, 200, 500), "unsupported quantity")
    require(fee["version"] and fee["pair_artifact_hash"] == pair["artifact_hash"] and
            uint(fee["valid_from_ns"]) <= now < uint(fee["valid_until_ns"]), "missing/expired fee quote")
    require(len(fee["fees"]) == len(fee["slippage"]) == 2, "two budgets required")
    fees, slips = [money(v) for v in fee["fees"]], [money(v) for v in fee["slippage"]]
    require(min(fees + slips) >= 0, "negative fee/reserve")
    costs = []
    values = [pair["artifact_hash"], quantity, fee["version"]]
    for i, book in enumerate(books):
        remaining, cost = quantity, 0
        for price, available in sorted(book["asks"].items()):
            take = min(available, remaining)
            cost = checked(cost + checked(price * take))
            remaining -= take
        require(remaining == 0, "insufficient depth")
        costs.append(cost)
        event = book["event"]
        values.extend((book["version"], book["seq"], book["recv"], event["source_ts_ns"],
                       event["raw_log_offset"], event["raw_hash"], book["depth_hash"], cost, fees[i], slips[i]))
    spend = checked(sum(costs + fees + slips))
    edge = checked(quantity * UNIT - spend)
    require(edge > 0, "nonpositive indicative edge")
    values.extend((250_000_000, 100_000_000, 500 * UNIT, 100 * UNIT, UNIT, 50, 1, 1,
                   fee["valid_from_ns"], fee["valid_until_ns"]))
    candidate = sha("".join(field(v) for v in values))
    eligible = (2 * quantity * UNIT <= 500 * UNIT and edge >= max(UNIT, (spend * 50 + 9999) // 10000)
                and all(costs[i] + fees[i] + slips[i] <= 100 * UNIT for i in range(2)))
    return {"candidate_id": candidate, "qty": quantity, "cost_yes_usd": decimal(costs[0]),
            "cost_no_usd": decimal(costs[1]), "fee_budget_usd": decimal(sum(fees)),
            "slippage_reserve_usd": decimal(sum(slips)), "quoted_edge_usd": decimal(edge),
            "risk_eligible": str(eligible).lower(), "artifact_hash": pair["artifact_hash"],
            "approval_revision": pair["approval_revision"], "fee_version": fee["version"],
            "yes_snapshot_version": books[0]["version"], "no_snapshot_version": books[1]["version"],
            "yes_raw_hash": books[0]["event"]["raw_hash"], "no_raw_hash": books[1]["event"]["raw_hash"]}


def scan(data):
    rows = []
    quantities = [f["qty"] for f in data["fee_quotes"]]
    require(len(quantities) == len(set(quantities)), "duplicate fee quantity")
    for scenario in data["scenarios"]:
        store = BookStore()
        for event in scenario["events"]:
            require(event["key"] in (data["pair"]["leg_yes"], data["pair"]["leg_no"]), "unmapped instrument")
            store.apply(event)
        for fee in data["fee_quotes"]:
            try:
                row = quote(data["pair"], store, fee, scenario["now_ns"])
            except ValueError:
                continue
            quantity = fee["qty"]
            per_leg = [money(row["cost_yes_usd"]), money(row["cost_no_usd"])]
            if any(per_leg[i] + money(fee["fees"][i]) + money(fee["slippage"][i]) > money(data["starting_cash"][i]) for i in range(2)):
                row["risk_eligible"] = "false"
            rows.append({"scenario": scenario["name"], "mode": "SIMULATED", **row})
    return rows


def audit_scenario(pair, scenario, starting_cash):
    validate_pair(pair)
    cutoff = uint(scenario["cutoff_ns"])
    quantities, spend, fees = [0, 0], [0, 0], 0
    seen, late = {}, 0
    receipts = []
    for fill in scenario["fills"]:
        require(fill["mode"] == "SIMULATED" and fill["pair_id"] == pair["pair_id"], "invalid receipt mode/pair")
        require(fill["canonical_payoff"] in ("YES", "NO"), "invalid payoff")
        i = 0 if fill["canonical_payoff"] == "YES" else 1
        require(fill["key"] == pair["leg_yes" if i == 0 else "leg_no"], "receipt key mismatch")
        require(fill["fill_id"] and fill["order_id"] and fill["fee_version"], "missing receipt evidence")
        raw = fill["raw_payload"]
        require(sha(raw) == fill["raw_hash"], "receipt raw checksum mismatch")
        require(json.loads(raw) == {k: v for k, v in fill.items() if k not in ("raw_hash", "raw_payload")}, "receipt raw/envelope mismatch")
        quantity, price, paid = qty(fill["qty"]), money(fill["price"]), money(fill["fee_paid"])
        require(quantity > 0 and 0 <= price <= UNIT and paid >= 0, "invalid fill")
        require(0 < uint(fill["exchange_time_ns"]) <= uint(fill["received_time_ns"]) <= cutoff, "reversed timestamp/cutoff")
        identity = field(fill["key"]["venue"]) + field(fill["fill_id"])
        if identity in seen:
            require(seen[identity] == fill, "conflicting duplicate fill")
            continue
        seen[identity] = fill
        spend[i] = checked(spend[i] + checked(quantity * price + paid))
        quantities[i] += quantity
        require(quantities[i] <= MAX_QTY and spend[i] <= money(starting_cash[i]), "account/inventory limit")
        fees = checked(fees + paid)
        late += fill["received_time_ns"] - fill["exchange_time_ns"] > 1_000_000_000
        receipts.append(fill)
    total = checked(sum(spend))
    floor = checked(min(quantities) * UNIT - total)
    outcome = scenario["illustrative_outcome"]
    require(outcome in ("YES", "NO", None), "invalid illustrative outcome")
    indicative = "" if outcome is None else decimal(quantities[0 if outcome == "YES" else 1] * UNIT - total)
    return {"scenario": scenario["name"], "mode": "SIMULATED", "qty_yes": quantities[0], "qty_no": quantities[1],
            "actual_spend_usd": decimal(total), "fees_paid_usd": decimal(fees),
            "payout_if_yes_usd": decimal(quantities[0] * UNIT), "payout_if_no_usd": decimal(quantities[1] * UNIT),
            "filled_locked_floor_usd": decimal(floor), "illustrative_settled_pnl_usd": indicative,
            "verified_live_realized_pnl_usd": "", "suspense_usd": decimal(0), "late_receipts": late,
            "artifact_hash": pair["artifact_hash"], "approval_revision": pair["approval_revision"],
            "cutoff_ns": cutoff, "fill_ids": ";".join(sorted(field(f["key"]["venue"]) + field(f["fill_id"]) for f in receipts)),
            "fee_versions": ";".join(sorted({f["fee_version"] for f in receipts})),
            "raw_references": ";".join(sorted(f'{f["key"]["venue"]}:{f["raw_log_offset"]}:{f["raw_hash"]}' for f in receipts))}


SCAN_FIELDS = ["scenario", "mode", "candidate_id", "qty", "cost_yes_usd", "cost_no_usd", "fee_budget_usd",
               "slippage_reserve_usd", "quoted_edge_usd", "risk_eligible", "artifact_hash", "approval_revision",
               "fee_version", "yes_snapshot_version", "no_snapshot_version", "yes_raw_hash", "no_raw_hash"]
AUDIT_FIELDS = ["scenario", "mode", "qty_yes", "qty_no", "actual_spend_usd", "fees_paid_usd", "payout_if_yes_usd",
                "payout_if_no_usd", "filled_locked_floor_usd", "illustrative_settled_pnl_usd", "verified_live_realized_pnl_usd",
                "suspense_usd", "late_receipts", "artifact_hash", "approval_revision", "cutoff_ns", "fill_ids", "fee_versions", "raw_references", "input_sha256"]


def write_csv(path, rows, fields):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

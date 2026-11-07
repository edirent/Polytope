"""Run the C++ composition twice and compare CSVs to independent Python math."""
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
REFERENCE = REPO / "research/cross_venue"
sys.path.insert(0, str(REFERENCE / "src"))
from reference import audit_scenario, load, scan, sha


def read(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def strings(rows):
    return [{k: str(v) for k, v in row.items()} for row in rows]


def main():
    binary = Path(sys.argv[1]).resolve()
    books = REFERENCE / "fixtures/synthetic_books.json"
    fills = REFERENCE / "fixtures/synthetic_fills.json"
    with tempfile.TemporaryDirectory(prefix="cross-venue-v0-") as temporary:
        base = Path(temporary)
        for name in ("a", "b"):
            subprocess.run([str(binary), "--input", str(books), "--simulated-fills", str(fills), "--out", str(base / name)], check=True)
        for path in (base / "a").iterdir():
            assert path.read_bytes() == (base / "b" / path.name).read_bytes(), f"nondeterministic {path.name}"
        assert read(base / "a/opportunities.csv") == strings(scan(load(books, "cross-venue-books-v0"))), "C++/Python opportunity mismatch"
        data = load(fills, "cross-venue-fills-v0")
        expected = [{**audit_scenario(data["pair"], case, data["starting_cash"]), "input_sha256": sha(fills.read_bytes())} for case in data["scenarios"]]
        assert read(base / "a/pnl_audit.simulated.csv") == strings(expected), "C++/Python receipt audit mismatch"
        manifest = json.loads((base / "a/manifest.json").read_text())
        assert manifest["live_orders_sent"] == 0 and manifest["verified_live_realized_pnl_usd"] is None
        subprocess.run([str(binary), "--input", str(books), "--out", str(base / "shadow")], check=True)
        assert read(base / "shadow/pnl_audit.simulated.csv") == [], "scan-only shadow must not invent fills"
        assert (base / "shadow/execution_events.simulated.jsonl").read_text() == ""
        policy_path = REPO / "config/cross_venue_v0.shadow.json"
        subprocess.run([str(binary), "--input", str(books), "--policy", str(policy_path), "--simulated-fills", str(fills), "--out", str(base / "configured")], check=True)
        assert read(base / "configured/opportunities.csv") == read(base / "a/opportunities.csv")
        assert read(base / "configured/pnl_audit.simulated.csv") == read(base / "a/pnl_audit.simulated.csv")
        risk = [json.loads(line) for line in (base / "a/risk_snapshots.simulated.jsonl").read_text().splitlines()]
        states = {r["scenario"]: r["state"] for r in risk}
        assert states["matched_100"] == "SETTLEMENT_PENDING"
        assert states["partial_hedge_60"] == "REHEDGE_OR_STOP"
        assert states["hedge_slippage"] == "FREEZE"
        assert all(not r["release_allowed"] for r in risk)
        live_policy = json.loads(policy_path.read_text())
        live_policy["live_enabled"] = True
        bad_policy = base / "bad-policy.json"
        bad_policy.write_text(json.dumps(live_policy))
        rejected = subprocess.run([str(binary), "--input", str(books), "--policy", str(bad_policy), "--out", str(base / "forbidden")], capture_output=True)
        assert rejected.returncode != 0 and not (base / "forbidden/manifest.json").exists()
        repeat = subprocess.run([str(binary), "--input", str(books), "--out", str(base / "a")], capture_output=True)
        assert repeat.returncode != 0, "existing audit artifacts cannot be overwritten"
        tampered = json.loads(books.read_text())
        tampered["scenarios"][0]["events"][0]["raw_payload"] += " "
        tampered_path = base / "tampered.json"
        tampered_path.write_text(json.dumps(tampered))
        rejected = subprocess.run([str(binary), "--input", str(tampered_path), "--out", str(base / "corrupt")], capture_output=True)
        assert rejected.returncode != 0 and not (base / "corrupt/manifest.json").exists()
        bad = subprocess.run([str(binary), "--live", "true"], capture_output=True)
        assert bad.returncode != 0, "live mode must not exist"
    print("C++/Python golden PnL, candidate/evidence parity, byte determinism and scan-only isolation passed")


if __name__ == "__main__":
    main()

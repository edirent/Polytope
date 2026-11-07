import argparse
from pathlib import Path
from reference import AUDIT_FIELDS, audit_scenario, load, sha, write_csv

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Audit SIMULATED receipts; verified live PnL is always empty")
    parser.add_argument("--input", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    data = load(args.input, "cross-venue-fills-v0")
    digest = sha(Path(args.input).read_bytes())
    rows = [{**audit_scenario(data["pair"], s, data["starting_cash"]), "input_sha256": digest} for s in data["scenarios"]]
    write_csv(args.out, rows, AUDIT_FIELDS)
    print(f"SIMULATED: audited {len(rows)} scenarios; no verified live realized PnL")

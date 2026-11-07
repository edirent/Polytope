import argparse
from reference import SCAN_FIELDS, load, scan, write_csv

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SYNTHETIC cross-venue L2 scanner; no order transport")
    parser.add_argument("--input", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    rows = scan(load(args.input, "cross-venue-books-v0"))
    write_csv(args.out, rows, SCAN_FIELDS)
    print(f"SIMULATED: {len(rows)} indicative candidates; live orders=0")

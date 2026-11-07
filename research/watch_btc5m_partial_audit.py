#!/usr/bin/env python3
"""Watch a live BTC 5m capture and refresh partial strategy audit.

The watcher is read-only with respect to the capture process. It only reads the
live samples/CLOB files and rewrites the partial audit artifacts whenever the
number of completed markets increases.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import subprocess
import sys
import time
from typing import Any


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def number(value: Any, default: float = math.nan) -> float:
    if value is None or value == "":
        return default
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    return out if math.isfinite(out) else default


def completed_market_count(samples_csv: Path, max_min_tau_sec: float) -> int:
    if not samples_csv.exists() or samples_csv.stat().st_size == 0:
        return 0
    by_market: dict[str, dict[str, float]] = {}
    with samples_csv.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            market = str(row.get("market_slug", ""))
            if not market:
                continue
            tau = number(row.get("tau_sec"))
            if not math.isfinite(tau):
                continue
            stats = by_market.setdefault(
                market,
                {"min_tau": math.inf, "max_tau": -math.inf},
            )
            stats["min_tau"] = min(stats["min_tau"], tau)
            stats["max_tau"] = max(stats["max_tau"], tau)
    return sum(
        1
        for stats in by_market.values()
        if stats["min_tau"] <= max_min_tau_sec and stats["max_tau"] >= 295.0
    )


def metadata_completed_count(metadata_path: Path) -> int:
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return 0
    return int(number(metadata.get("completed_market_count"), 0))


def write_state(path: Path, state: dict[str, Any]) -> None:
    path.write_text(json.dumps(state, indent=2), encoding="utf-8")


def run_command(cmd: list[str], cwd: Path) -> None:
    print(f"{now_iso()} run {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def refresh_audit(args: argparse.Namespace, completed_count: int) -> None:
    run_dir = Path(args.run_dir)
    repo_root = Path(args.repo_root)
    run_command(
        [
            sys.executable,
            "research/finalize_partial_btc5m_capture.py",
            "--run-dir",
            str(run_dir),
            "--max-min-tau-sec",
            str(args.max_min_tau_sec),
            "--settlement-timeout-sec",
            str(args.settlement_timeout_sec),
            "--settlement-poll-sec",
            str(args.settlement_poll_sec),
        ],
        repo_root,
    )
    run_command(
        [
            sys.executable,
            "research/analyze_sigma65_fixed_versions.py",
            "--run-dir",
            str(run_dir),
            "--samples-file",
            "samples_enriched_partial_complete_markets.csv",
            "--clob-file",
            "clob_snapshots.jsonl",
            "--out-file",
            "sigma65_fixed_versions_trade_audit_partial_current.json",
            "--report-file",
            "sigma65_fixed_versions_trade_audit_partial_current.md",
        ],
        repo_root,
    )
    print(f"{now_iso()} refreshed completed_markets={completed_count}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--repo-root", default="/home/edirent/Polytope")
    parser.add_argument("--poll-sec", type=float, default=10.0)
    parser.add_argument("--max-min-tau-sec", type=float, default=2.0)
    parser.add_argument("--settlement-timeout-sec", type=int, default=90)
    parser.add_argument("--settlement-poll-sec", type=int, default=5)
    args = parser.parse_args()

    run_dir = Path(args.run_dir)
    samples_csv = run_dir / "samples.csv"
    metadata_path = run_dir / "partial_complete_markets_metadata.json"
    state_path = run_dir / "audit_watcher_state.json"

    last_attempted = metadata_completed_count(metadata_path)
    print(
        f"{now_iso()} watcher_start run_dir={run_dir} "
        f"last_audited={last_attempted} poll_sec={args.poll_sec}",
        flush=True,
    )
    write_state(state_path, {
        "status": "running",
        "started_at_utc": now_iso(),
        "last_audited_completed_markets": last_attempted,
    })

    while True:
        try:
            completed = completed_market_count(samples_csv, args.max_min_tau_sec)
            audited = metadata_completed_count(metadata_path)
            target = max(last_attempted, audited)
            if completed > target:
                print(
                    f"{now_iso()} detected completed_markets={completed} "
                    f"audited={audited}",
                    flush=True,
                )
                write_state(state_path, {
                    "status": "refreshing",
                    "updated_at_utc": now_iso(),
                    "completed_markets": completed,
                    "audited_markets_before": audited,
                })
                refresh_audit(args, completed)
                last_attempted = completed
                audited_after = metadata_completed_count(metadata_path)
                write_state(state_path, {
                    "status": "running",
                    "updated_at_utc": now_iso(),
                    "completed_markets": completed,
                    "last_audited_completed_markets": audited_after,
                })
            time.sleep(args.poll_sec)
        except KeyboardInterrupt:
            raise
        except Exception as exc:
            print(f"{now_iso()} watcher_error {type(exc).__name__}: {exc}", flush=True)
            write_state(state_path, {
                "status": "error",
                "updated_at_utc": now_iso(),
                "error": f"{type(exc).__name__}: {exc}",
            })
            time.sleep(max(args.poll_sec, 10.0))


if __name__ == "__main__":
    raise SystemExit(main())

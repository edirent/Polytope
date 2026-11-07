"""Bounded orchestration of native read-only capture segments; never sums PnL.

Each segment is an independent shadow experiment. Disconnect/rebuild boundaries
are explicit; this is not a claim of uninterrupted streaming or one continuous
capital-constrained portfolio backtest across segments.
"""
import argparse
import json
import os
import signal
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path


def tree_bytes(root):
    return sum(p.stat().st_size for p in root.rglob("*") if p.is_file())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/run_cross_venue_live_shadow"))
    parser.add_argument("--config", type=Path, default=Path("config/cross_venue_v0_1.json"))
    parser.add_argument("--hours", type=float, default=72)
    parser.add_argument("--segment-minutes", type=int, default=30)
    parser.add_argument("--max-total-gb", type=float, default=8)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--require-research-ready", action="store_true", help="Refuse a long research campaign without approved revisions and authenticated Kalshi L2")
    parser.add_argument("--preflight-only", action="store_true", help="Save readiness evidence without starting a capture")
    args = parser.parse_args()
    if not (0 < args.hours <= 72 and 1 <= args.segment_minutes <= 60 and args.max_total_gb >= 0.25):
        parser.error("hours must be <=72, segment minutes 1..60, and budget >=0.25 GiB")
    args.out.mkdir(parents=True, exist_ok=False)
    original = json.loads(args.config.read_text())
    if args.require_research_ready or args.preflight_only:
        approvals = json.loads(Path(original['approved_pairs']).read_text())
        pairs = [p for p in approvals['pairs'] if p.get('enabled') and not p.get('synthetic',True)]
        review_path=original.get('human_review_manifest')
        reviews=json.loads(Path(review_path).read_text()).get('reviews',[]) if review_path and Path(review_path).exists() else []
        explicitly_bound=[p for p in pairs if any(r.get('candidate_pair_id')==p.get('pair_id') and
            r.get('status')=='APPROVED' and r.get('decision')=='APPROVED' and r.get('reviewer') and r.get('reviewed_at') and
            r.get('approved_revision')==p.get('approval_revision') and r.get('approved_artifact_hash')==p.get('artifact_hash') and
            r.get('coverage_proven') and r.get('settlement_assets_reviewed') and r.get('quantity_constraints_reviewed') for r in reviews)]
        credentials = bool(os.getenv('KALSHI_API_KEY_ID')) and bool(os.getenv('KALSHI_PRIVATE_KEY_PATH') or os.getenv('KALSHI_PRIVATE_KEY'))
        reasons=[]
        if not pairs:reasons.append('NO_EXPLICITLY_APPROVED_PAIRS')
        elif not explicitly_bound:reasons.append('NO_APPROVAL_BOUND_TO_EXACT_HUMAN_REVIEW_REVISION')
        if not credentials:reasons.append('MISSING_KALSHI_AUTHENTICATED_WS_CREDENTIALS')
        if not original.get('require_explicit_revision_review'):reasons.append('EXPLICIT_REVISION_GATE_NOT_ENABLED')
        preflight={'status':'BLOCKED' if reasons else 'STRUCTURAL_PREFLIGHT_PASSED_NATIVE_VALIDATION_STILL_REQUIRED',
                   'reasons':reasons,'enabled_approval_records':len(pairs),'kalshi_credentials_present':credentials,
                   'requested_hours':args.hours,'collection_started':False,'verified_live_realized_pnl':None}
        (args.out/'preflight.json').write_text(json.dumps(preflight,indent=2)+'\n')
        if reasons or args.preflight_only:
            print(json.dumps(preflight,indent=2))
            return 2 if reasons else 0
    ceiling = int(args.max_total_gb * 1024**3)
    deadline = time.monotonic() + args.hours * 3600
    manifest = {"schema": "cross-venue-bounded-campaign-v0.1", "started_utc": datetime.now(timezone.utc).isoformat(),
                "requested_hours": args.hours, "max_total_bytes": ceiling, "segments": [],
                "evidence": "PER_SEGMENT_REAL_CAPTURE_AND_EXPLICIT_TRANSPORT_GAPS",
                "portfolio_scope": "Independent per-segment experiments; no cross-segment capital reuse or PnL aggregation is asserted",
                "verified_live_realized_pnl": None}
    status = "TIME_LIMIT_REACHED"
    try:
        while time.monotonic() < deadline:
            remaining = ceiling - tree_bytes(args.out)
            if remaining < 100 * 1024**2:
                status = "STORAGE_BUDGET_REACHED"
                break
            index = len(manifest["segments"])
            config = dict(original)
            if args.require_research_ready:config['require_research_ready']=True
            config["max_raw_bytes"] = min(config.get("max_raw_bytes", 256 * 1024**2), remaining - 64 * 1024**2)
            config_path = args.out / f"config.{index:04d}.json"
            config_path.write_text(json.dumps(config, indent=2) + "\n")
            run = args.out / f"segment_{index:04d}"
            duration = max(1, min(args.segment_minutes * 60, int(deadline - time.monotonic())))
            command = [str(args.binary.resolve()), "--config", str(config_path), "--duration-sec", str(duration), "--run-dir", str(run),
                       "--capital", str(config.get("capital", "200000"))]
            with (args.out / f"segment_{index:04d}.log").open("w") as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    while process.poll() is None:
                        if time.monotonic() >= deadline or tree_bytes(args.out) >= ceiling:
                            process.send_signal(signal.SIGINT)
                            try:
                                process.wait(timeout=60)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
                            break
                        time.sleep(5)
                except KeyboardInterrupt:
                    process.send_signal(signal.SIGINT)
                    try:
                        process.wait(timeout=60)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    status = "OPERATOR_INTERRUPTED"
                    raise
            entry = {"directory": str(run), "return_code": process.returncode,
                     "has_capture_manifest": (run / "capture_manifest.json").exists(),
                     "has_research_report": (run / "analysis/report.md").exists()}
            manifest["segments"].append(entry)
            if process.returncode:
                status = "SEGMENT_FAILED_REVIEW_LOG"
                break
    except KeyboardInterrupt:
        pass
    finally:
        manifest["status"] = status
        manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
        manifest["bytes_used"] = tree_bytes(args.out)
        (args.out / "campaign_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    raise SystemExit(main())

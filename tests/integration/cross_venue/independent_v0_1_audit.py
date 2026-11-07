import subprocess
import sys
import tempfile
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "research/cross_venue"))
from audit_v0_1 import audit

with tempfile.TemporaryDirectory(prefix="polytope-independent-audit-") as temporary:
    root = Path(temporary) / "native"
    subprocess.run([sys.argv[1], "--keep-output", str(root)], check=True)
    result = audit(root / "lab", root / "analysis-a")
    assert result["simulated_trades_recomputed"] > 0, "audit must exercise actual simulated receipts, not an empty report"
    assert result["data_evidence"] == "SYNTHETIC_REGRESSION"
    path = root / "analysis-a/summary.json"
    original = path.read_bytes()
    invalid = json.loads(original)
    invalid["verified_live_realized_pnl"] = 0
    path.write_text(json.dumps(invalid))
    try:
        audit(root / "lab", root / "analysis-a")
    except ValueError:
        pass
    else:
        raise AssertionError("zero must not replace null verified live PnL")
    path.write_bytes(original)
    print(result)

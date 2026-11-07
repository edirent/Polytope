"""Real historical replay and independent audit; temporary deterministic outputs."""
import json,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'research/cross_venue'))
from audit_v0_1_2 import audit
source=ROOT/'runs/cross_venue/v0_1_1_authenticated_20261105_3/capture'
if not source.is_dir():
    print('Historical real capture unavailable: NOT EXECUTED');raise SystemExit(77)
with tempfile.TemporaryDirectory(prefix='polytope-v012-audit-') as directory:
    outputs=[Path(directory)/str(i) for i in range(2)]
    for out in outputs:subprocess.run([sys.argv[1],'--run-dir',str(source),'--out',str(out)],check=True,cwd=ROOT,stdout=subprocess.DEVNULL)
    result=audit(source,outputs[0],outputs[1]);assert result['deterministic_files_compared']>=14
    print(json.dumps(result))

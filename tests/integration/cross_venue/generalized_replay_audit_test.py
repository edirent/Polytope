"""Independent real raw replay, optimization and byte determinism regression."""
import json,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'research/cross_venue'))
from audit_v0_1_3 import audit
proofs=ROOT/'runs/cross_venue/v0_1_3_20261106/proofs/payoff_proofs.jsonl'
source=ROOT/'runs/cross_venue/v0_1_2_focused_20261106_2/capture'
if not proofs.is_file() or not source.is_dir():
    print('Real frozen generalized proof/capture evidence unavailable: NOT EXECUTED');raise SystemExit(77)
with tempfile.TemporaryDirectory(prefix='polytope-v013-real-audit-') as directory:
    outputs=[Path(directory)/str(i) for i in range(2)]
    for out in outputs:
        subprocess.run([sys.argv[1],'--phase','replay','--config','config/cross_venue_v0_1_3.json','--relationships',str(proofs),'--run-dir',str(source),'--out',str(out)],check=True,cwd=ROOT,stdout=subprocess.DEVNULL)
    result=audit(proofs,source,outputs[0],outputs[1]);assert result['deterministic_files_compared']>=15 and result['portfolio_states_verified']>0
    print(json.dumps(result))

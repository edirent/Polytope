"""Real-book quantity breakpoints, coupled fee lower bound and determinism."""
import csv,json,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'research/cross_venue'))
from audit_v0_1_3_1 import quantity_audit
proofs=ROOT/'runs/cross_venue/v0_1_3_20261106/proofs/payoff_proofs.jsonl'
original=ROOT/'runs/cross_venue/v0_1_3_20261106/analysis/historical_v011'
source=ROOT/'runs/cross_venue/v0_1_1_authenticated_20261105_3/capture'
if not proofs.is_file() or not original.is_dir() or not source.is_dir():raise SystemExit(77)
rows=list(csv.DictReader((original/'portfolio_candidates.csv').open()));chosen=next(r['portfolio_id'] for r in rows if r['any_gross_positive']=='true')
with tempfile.TemporaryDirectory(prefix='polytope-v0131-real-quantity-') as tmp:
    tmp=Path(tmp);cfg=json.loads((ROOT/'config/cross_venue_v0_1_3.json').read_text());cfg['quantity_verification_portfolio_ids']=[chosen];cfg['quantity_verification_expand_to_visible_depth']=True;(tmp/'config.json').write_text(json.dumps(cfg));outputs=[]
    for n in [0,1]:
        out=tmp/str(n);outputs.append(out);subprocess.run([sys.argv[1],'--phase','replay','--config',str(tmp/'config.json'),'--relationships',str(proofs),'--run-dir',str(source),'--out',str(out)],cwd=ROOT,stdout=subprocess.DEVNULL,check=True)
    result=quantity_audit(proofs,outputs[0]);assert result['quantity_states_checked']==1 and result['coupled_continuous_bounds_verified']==1
    for p in outputs[0].iterdir():
        if p.name!='quantity_solver_timings.jsonl':assert p.read_bytes()==(outputs[1]/p.name).read_bytes(),p.name
    print(json.dumps(result));print('Core outputs deterministic; measured solver times intentionally stored separately.')

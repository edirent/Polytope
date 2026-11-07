"""One finite 1200s native generalized-market capture and read-only analysis."""
import argparse,hashlib,json,subprocess
from datetime import datetime,timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def run(out):
    config=ROOT/'config/cross_venue_v0_1_3.capture.json';cfg=json.loads(config.read_text());proofs=ROOT/cfg['generalized_relationships']
    if hashlib.sha256(proofs.read_bytes()).hexdigest()!=cfg['generalized_relationships_sha256']:raise ValueError('proof selection changed')
    out.mkdir(parents=True,exist_ok=False)
    commands=[('capture',['build/run_cross_venue_live_shadow','--config',str(config),'--duration-sec','1200','--run-dir',str(out/'capture')],1500),('feed_audit',['build/cross_venue_feed_audit','--run-dir',str(out/'capture'),'--out',str(out/'feed_audit')],180),('generalized',['build/run_generalized_payoff_research','--phase','replay','--config',str(config),'--relationships',str(proofs),'--run-dir',str(out/'capture'),'--out',str(out/'generalized')],600)]
    for stage,command,timeout in commands:
        with (out/(stage+'.log')).open('w') as log:r=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
        (out/'workflow_status.json').write_text(json.dumps({'stage':stage,'exit_code':r.returncode,'credentials_persisted':False})+'\n')
        if r.returncode:raise ValueError('bounded native stage failed')
    (out/'workflow_status.json').write_text(json.dumps({'status':'BOUNDED_GENERALIZED_CAPTURE_FINISHED','credentials_persisted':False})+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    try:run(a.out)
    except (OSError,ValueError,subprocess.TimeoutExpired):raise SystemExit('Bounded generalized research stopped; credential contents omitted.')

"""One bounded 1200s read-only native capture, followed by audit and economics."""
import argparse,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def run(out):
    out.mkdir(parents=True,exist_ok=False)
    stages=[('capture',['build/run_cross_venue_live_shadow','--config','config/cross_venue_v0_1_2.json','--duration-sec','1200','--run-dir',str(out/'capture')],1500),
            ('feed_audit',['build/cross_venue_feed_audit','--run-dir',str(out/'capture'),'--out',str(out/'feed_audit')],180),
            ('economics',['build/cross_venue_economic_discovery','--run-dir',str(out/'capture'),'--out',str(out/'economics')],600)]
    for stage,command,bound in stages:
        with (out/(stage+'.log')).open('w') as log:
            r=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,timeout=bound)
        (out/'workflow_status.json').write_text(json.dumps({'stage':stage,'exit_code':r.returncode,'credentials_persisted':False})+'\n')
        if r.returncode:raise ValueError('bounded native stage failed')
    (out/'workflow_status.json').write_text(json.dumps({'status':'BOUNDED_FOCUSED_CAPTURE_FINISHED','credentials_persisted':False})+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    try:run(a.out)
    except (OSError,ValueError,subprocess.TimeoutExpired):raise SystemExit('Bounded research stopped; credential contents omitted.')

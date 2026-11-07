"""Bounded two-stage native WS validation with inherited in-memory credentials."""
import argparse
import json
import os
import subprocess
import time
from pathlib import Path


def ready(path,seconds):
    deadline=time.monotonic()+seconds
    while not path.exists():
        if time.monotonic()>=deadline:raise TimeoutError('bounded validation readiness wait expired')
        time.sleep(.5)
    path.unlink()


def run(args):
    args.out.mkdir(parents=True,exist_ok=False)
    if not os.environ.get('KALSHI_API_KEY_ID'):raise ValueError('required local environment absent')
    if not args.automatic:ready(Path(str(args.ready_prefix)+'.smoke'),600)
    with (args.out/'key_check.json').open('w') as stream:
        check=subprocess.run(['build/cross_venue_key_check'],stdout=stream,stderr=subprocess.DEVNULL)
    if check.returncode:raise ValueError('native private key compatibility check failed')
    for stage,duration in [('smoke',60),('capture',600)]:
        if stage=='capture' and not args.automatic:ready(Path(str(args.ready_prefix)+'.capture'),900)
        for attempt in range(2 if stage=='smoke' and args.automatic else 1):
            label=stage if not attempt else stage+'_retry_1'
            config=json.loads(args.config.read_text());config['diagnostic_reconnect_after_sec']=0 if stage=='smoke' else 300
            config_path=args.out/(label+'.config.json');config_path.write_text(json.dumps(config,indent=2)+'\n')
            command=['build/run_cross_venue_live_shadow','--config',str(config_path),'--duration-sec',str(duration),'--run-dir',str(args.out/label)]
            with (args.out/(label+'.log')).open('w') as log:
                result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=duration+240)
            print(json.dumps({'stage':label,'exit_code':result.returncode,'capture_manifest_saved':(args.out/label/'capture_manifest.json').exists()}),flush=True)
            if result.returncode:raise ValueError('native capture failed; inspect sanitized research log')
            audit=args.out/(label+'_feed_audit')
            with (args.out/(label+'_feed_audit.log')).open('w') as stream:
                process=subprocess.run(['build/cross_venue_feed_audit','--run-dir',str(args.out/label),'--out',str(audit)],stdout=stream,stderr=subprocess.STDOUT,timeout=120)
            if process.returncode:raise ValueError('native feed audit failed')
            health=json.loads((audit/'summary.json').read_text())
            verified=health['kalshi_snapshot_and_delta_observed']
            print(json.dumps({'stage':label,'kalshi_real_snapshot_and_delta_observed':verified,'both_venues_healthy_l2_sec':health['both_venues_any_asset_healthy_l2_sec']}),flush=True)
            if verified:break
            if stage=='smoke' and args.automatic and attempt==0:
                # Keep credentials only in this existing process for a bounded
                # operator-requested retry after a parser fix. Never write them.
                status={'status':'WAITING_FOR_BOUNDED_SMOKE_RETRY','retry_request_file':str(args.out/'retry_smoke.request'),
                    'wait_limit_sec':600,'credentials_persisted':False,'real_snapshot_and_delta_verified':False}
                (args.out/'workflow_status.json').write_text(json.dumps(status,indent=2)+'\n')
                print(json.dumps(status),flush=True);ready(args.out/'retry_smoke.request',600)
            else:raise ValueError('signed session did not produce real snapshot and delta; full feed validation remains incomplete')
    (args.out/'workflow_status.json').write_text(json.dumps({'status':'BOUNDED_CAPTURE_AND_AUDITS_FINISHED','credentials_persisted':False})+'\n')
    return 0


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--config',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--ready-prefix',type=Path)
    p.add_argument('--automatic',action='store_true',help='Run the bounded capture automatically only after smoke proves real snapshot and delta')
    a=p.parse_args()
    if not a.automatic and not a.ready_prefix:p.error('--ready-prefix or --automatic is required')
    try:raise SystemExit(run(a))
    except (OSError,ValueError,TimeoutError,subprocess.TimeoutExpired):
        raise SystemExit('Authenticated validation stopped; credential contents omitted.')

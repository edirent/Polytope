#!/usr/bin/env python3
"""Secure local environment launch; credentials stay in process memory, never argv/log/files."""
import argparse,getpass,hashlib,json,os
from pathlib import Path
import subprocess,sys,time
ROOT=Path(__file__).resolve().parents[1]
READY=ROOT/'runs/runtime_v020/release_ready.json'

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def worker():
    deadline=time.monotonic()+1800
    while not READY.exists():
        if time.monotonic()>=deadline:raise RuntimeError('RELEASE_READY_TIMEOUT_NO_CAPTURE_STARTED')
        time.sleep(.5)
    ready=json.loads(READY.read_text());binary=Path(ready['binary'])
    if sha(binary)!=ready['binary_sha256']:raise RuntimeError('FROZEN_BINARY_HASH_MISMATCH')
    processes=[]
    for item in ready['experiments']:
        cfg=Path(item['config'])
        if sha(cfg)!=item['config_sha256']:raise RuntimeError('FROZEN_CONFIG_HASH_MISMATCH')
        log=Path(item['log']);log.parent.mkdir(parents=True,exist_ok=True)
        with log.open('wb') as out:
            p=subprocess.Popen([str(binary),'--config',str(cfg)],cwd=ROOT,env=os.environ,stdout=out,stderr=subprocess.STDOUT)
        processes.append((item['mode'],p))
    results=[]
    try:
        for mode,p in processes:
            code=p.wait(timeout=2100);results.append({'mode':mode,'returncode':code})
    finally:
        for _,p in processes:
            if p.poll() is None:p.terminate()
        for _,p in processes:
            if p.poll() is None:
                try:p.wait(timeout=20)
                except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
    for item in ready['experiments']:
        cfg=json.loads(Path(item['config']).read_text());env={k:v for k,v in os.environ.items() if not k.startswith('KALSHI_')}
        with (Path(item['log']).with_suffix('.audit.log')).open('w') as audit_log:
            audited=subprocess.run([sys.executable,str(ROOT/'tools/audit_operational_run.py'),'--run',cfg['output_dir'],
                '--binary',str(binary),'--with-replay'],cwd=ROOT,env=env,stdout=audit_log,stderr=subprocess.STDOUT,timeout=1500)
        for result in results:
            if result['mode']==item['mode']:result['audit_returncode']=audited.returncode
    (ROOT/'runs/runtime_v020/local_launch_results.json').write_text(json.dumps({'experiments':results,'credential_persistence':False},indent=2)+'\n')

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--worker',action='store_true');args=parser.parse_args()
    if args.worker:worker();return
    if not sys.stdin.isatty() and not os.environ.get('KALSHI_API_KEY_ID'):raise RuntimeError('RUN_FROM_CONFIGURED_WSL_TERMINAL')
    if not os.environ.get('KALSHI_API_KEY_ID'):
        os.environ['KALSHI_API_KEY_ID']=getpass.getpass('Kalshi Key ID (hidden, local memory only): ').strip()
    if not os.environ.get('KALSHI_API_KEY_ID'):raise RuntimeError('MISSING_LOCAL_KEY_ID')
    os.environ.setdefault('KALSHI_PRIVATE_KEY_PATH','/home/edirent/.secrets/kalshi.pem')
    if not os.access(os.environ['KALSHI_PRIVATE_KEY_PATH'],os.R_OK):raise RuntimeError('PRIVATE_KEY_NOT_READABLE')
    root=ROOT/'runs/runtime_v020';root.mkdir(parents=True,exist_ok=True)
    # Prevent duplicate staging workers, without persisting any environment value.
    import fcntl
    lock=open(root/'launcher.lock','w');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    with (root/'local_launcher.log').open('ab') as log:
        subprocess.Popen([sys.executable,str(Path(__file__).resolve()),'--worker'],cwd=ROOT,env=os.environ,
            stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True,pass_fds=(lock.fileno(),))
    print('Secure bounded validation staged. Credentials remain in local process memory.')
    print('It waits at most 30 minutes for a verified release, then runs PAPER and SHADOW for 30 minutes each in parallel.')
if __name__=='__main__':
    try:main()
    except Exception as exc:
        # Never stringify exception payloads from credential handling.
        print('Local launcher stopped:',type(exc).__name__,file=sys.stderr);sys.exit(1)

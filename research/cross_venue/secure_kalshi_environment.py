"""Launch bounded native validation in the configured terminal environment.

Preferred action: launch. No socket or credential transfer is needed.
The optional legacy transfer keeps values in process memory over a same-UID
0600 socket. No action persists key IDs, PEM contents or signatures.
"""
import argparse
import json
import os
import socket
import stat
import struct
import subprocess
import sys
import getpass
import warnings
from pathlib import Path

SOCKET=Path('/home/edirent/.secrets/polytope-kalshi-env.sock')
ROOT=Path(__file__).resolve().parents[2]


class TransferError(ValueError):
    """Static actionable error code; never include credential contents."""


def launch(economic=False,generalized=False):
    """Use the terminal's environment locally; no credential transfer needed."""
    env=dict(os.environ)
    key_id=env.get('KALSHI_API_KEY_ID','').strip()
    if not key_id:
        if not sys.stdin.isatty():raise TransferError('KEY_ID_NOT_EXPORTED: run launch in an interactive WSL terminal; a hidden local prompt is available there')
        try:
            with warnings.catch_warnings():
                warnings.simplefilter('error',getpass.GetPassWarning)
                key_id=getpass.getpass('Kalshi Key ID (hidden local input; not saved): ').strip()
        except (getpass.GetPassWarning,EOFError,KeyboardInterrupt):
            raise TransferError('HIDDEN_INPUT_UNAVAILABLE: export the existing Key ID in an interactive WSL terminal; contents omitted')
    if not key_id or '\n' in key_id or '\r' in key_id:raise TransferError('KEY_ID_MISSING_OR_INVALID: contents omitted')
    key_path=env.get('KALSHI_PRIVATE_KEY_PATH') or '/home/edirent/.secrets/kalshi.pem'
    if not Path(key_path).is_file() or not os.access(key_path,os.R_OK):raise TransferError('LOCAL_KEY_FILE_NOT_READABLE: check KALSHI_PRIVATE_KEY_PATH')
    env.pop('KALSHI_PRIVATE_KEY',None);env.update(KALSHI_API_KEY_ID=key_id,KALSHI_PRIVATE_KEY_PATH=key_path)
    for binary in ['cross_venue_key_check','run_cross_venue_live_shadow','cross_venue_feed_audit']:
        if not (ROOT/'build'/binary).is_file():raise TransferError('NATIVE_BINARY_MISSING: build the existing Cross-Venue targets first')
    if generalized and (not (ROOT/'build/run_generalized_payoff_research').is_file() or not (ROOT/'config/cross_venue_v0_1_3.capture.json').is_file()):raise TransferError('GENERALIZED_PREFLIGHT_NOT_READY: prepare native proofs and capture configuration first')
    base=ROOT/('runs/cross_venue/v0_1_3_focused_20261106' if generalized else ('runs/cross_venue/v0_1_2_focused_20261106' if economic else 'runs/cross_venue/v0_1_1_authenticated_20261105'));out=base;index=1
    while out.exists():index+=1;out=base.with_name(base.name+'_'+str(index))
    out.parent.mkdir(parents=True,exist_ok=True)
    log_path=out.with_name(out.name+'.workflow.log')
    with log_path.open('x') as log:
        command=([sys.executable,str(ROOT/'research/cross_venue/generalized_capture_workflow.py'),'--out',str(out)] if generalized else [sys.executable,str(ROOT/'research/cross_venue/economic_capture_workflow.py'),'--out',str(out)] if economic else [sys.executable,str(ROOT/'research/cross_venue/authenticated_ws_workflow.py'),'--config',str(ROOT/'config/cross_venue_authenticated_l2.validation.json'),'--out',str(out),'--automatic'])
        child=subprocess.Popen(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    # Status contains paths and PID only, never the environment or credentials.
    status={'pid':child.pid,'output_directory':str(out),'workflow_log':str(log_path),
            'mode':('BOUNDED_NATIVE_READ_ONLY_1200S_GENERALIZED_CAPTURE' if generalized else ('BOUNDED_NATIVE_READ_ONLY_1200S_FOCUSED_CAPTURE' if economic else 'BOUNDED_NATIVE_READ_ONLY_60S_SMOKE_THEN_600S_CAPTURE')),'credentials_persisted':False}
    (out.parent/'authenticated_ws_launch.latest.json').write_text(json.dumps(status,indent=2)+'\n')
    print('Bounded native validation launched. Credentials stay in local process memory; no socket is required.')
    print('Evidence directory: '+str(out))


def same_user(connection):
    _,uid,_=struct.unpack('3i',connection.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
    if uid!=os.getuid():raise ValueError('local peer UID mismatch')


def send(path):
    if not path.exists():raise TransferError('RECEIVER_NOT_RUNNING_OR_EXPIRED: use launch instead; no socket is required')
    info=path.stat()
    if info.st_uid!=os.getuid() or stat.S_IMODE(info.st_mode)!=0o600 or not stat.S_ISSOCK(info.st_mode):
        raise TransferError('SOCKET_OWNER_OR_PERMISSIONS_INVALID: use launch from the same WSL user')
    key_id=os.environ.get('KALSHI_API_KEY_ID','').strip()
    if not key_id:raise TransferError('KEY_ID_NOT_EXPORTED: export the existing variable, or use launch for a hidden local prompt')
    values={'KALSHI_API_KEY_ID':key_id,'KALSHI_PRIVATE_KEY_PATH':os.environ.get('KALSHI_PRIVATE_KEY_PATH') or '/home/edirent/.secrets/kalshi.pem'}
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
        connection.settimeout(10);connection.connect(str(path));same_user(connection)
        connection.sendall(json.dumps(values).encode());connection.shutdown(socket.SHUT_WR)
        if connection.recv(32)!=b'ACCEPTED':raise ValueError('environment transfer was not accepted')
    print('Local environment delivered; credential values were not displayed or saved.')


def receive(path,timeout,command):
    if path.exists():raise ValueError('socket path already exists; do not replace another listener')
    path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as listener:
        old=os.umask(0o177)
        try:listener.bind(str(path))
        finally:os.umask(old)
        listener.listen(1);listener.settimeout(timeout)
        print('Local secure environment listener ready; bounded wait, no credential persistence.',flush=True)
        try:
            connection,_=listener.accept()
            with connection:
                same_user(connection);connection.settimeout(10);data=b''
                while True:
                    block=connection.recv(4096)
                    if not block:break
                    data+=block
                    if len(data)>8192:raise ValueError('environment payload exceeds bound')
                values=json.loads(data)
                if set(values)!={'KALSHI_API_KEY_ID','KALSHI_PRIVATE_KEY_PATH'} or not all(isinstance(v,str) and v and '\x00' not in v for v in values.values()):
                    raise ValueError('invalid local environment payload')
                if not Path(values['KALSHI_PRIVATE_KEY_PATH']).is_file():raise ValueError('local private key file absent')
                env=dict(os.environ);env.pop('KALSHI_PRIVATE_KEY',None);env.update(values)
                connection.sendall(b'ACCEPTED')
        finally:path.unlink(missing_ok=True)
    # No shell interpolation, credentials in argv, or environment dumps.
    return subprocess.run(command,env=env).returncode


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['send','receive','launch','launch-economic','launch-generalized'])
    p.add_argument('--socket',type=Path,default=SOCKET);p.add_argument('--timeout-sec',type=int,default=1200)
    p.add_argument('command',nargs=argparse.REMAINDER);a=p.parse_args()
    try:
        if a.action in ('launch','launch-economic','launch-generalized'):launch(a.action=='launch-economic',a.action=='launch-generalized')
        elif a.action=='send':send(a.socket)
        else:
            command=a.command[1:] if a.command[:1]==['--'] else a.command
            if not command or not 1<=a.timeout_sec<=1800:p.error('bounded timeout and child command required')
            raise SystemExit(receive(a.socket,a.timeout_sec,command))
    except TransferError as error:
        raise SystemExit(str(error))
    except (OSError,ValueError,json.JSONDecodeError):
        raise SystemExit('Secure local transfer failed or timed out; credential contents omitted.')

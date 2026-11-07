#!/usr/bin/env python3
"""Executable fault tests against the real native runtime, no exchange access."""
import argparse
import copy
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
import urllib.request
import urllib.error
import socket

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--source', required=True)
parser.add_argument('--backend')
parser.add_argument('--evidence-dir')
args, remaining = parser.parse_known_args()
BINARY = str(Path(args.binary).resolve())
SOURCE = Path(args.source).resolve()

class RuntimeContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='polytope-runtime-')
        self.root = Path(self.temp.name)
        self.feed = [json.loads(x) for x in (SOURCE/'tests/fixtures/runtime/books.jsonl').read_text().splitlines()]
        self.cfg = json.loads((SOURCE/'config/trading_runtime.replay.json').read_text())
        self.cfg.update(input=str(self.root/'feed.jsonl'), output_dir=str(self.root/'out'),
                        oracle_artifact=str(SOURCE/'tests/fixtures/runtime/oracle_artifact'),
                        kill_switch_file=str(self.root/'KILL'), allow_test_faults=True)
        self.config = self.root/'config.json'
        self.results = []

    def tearDown(self):
        if args.evidence_dir:
            target = Path(args.evidence_dir)/self._testMethodName
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(self.root, target)
            (target/'command_results.json').write_text(json.dumps(self.results, indent=2))
        self.temp.cleanup()

    def save(self):
        Path(self.cfg['input']).write_text(''.join(json.dumps(x)+'\n' for x in self.feed))
        self.config.write_text(json.dumps(self.cfg, indent=2)+'\n')

    def run_native(self, recover=False, code=0):
        command = [BINARY, '--config', str(self.config)] + (['--recover'] if recover else [])
        p = subprocess.run(command, cwd=SOURCE, capture_output=True, text=True, timeout=30)
        self.results.append(dict(command=command, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr))
        self.assertEqual(p.returncode, code, p.stderr)
        return p

    def dashboard(self):
        return json.loads((Path(self.cfg['output_dir'])/'dashboard.json').read_text())

    def events(self):
        return [json.loads(x)['event'] for x in (Path(self.cfg['output_dir'])/'events.jsonl').read_text().splitlines()]

    def event_count(self, kind):
        return sum(e['type'] == kind for e in self.events())

    def test_three_modes_share_decision_and_risk(self):
        counts=[]
        for mode in ['REPLAY','PAPER','SHADOW']:
            self.cfg.update(mode=mode,output_dir=str(self.root/mode))
            self.save(); self.run_native()
            d=self.dashboard()['runtime']; counts.append(d['approvals'])
            self.assertEqual(d['approvals'],1)
            self.assertFalse(d['live_enabled'])
            self.assertIsNone(d['verified_live_realized_pnl'])
            self.assertEqual(d['active_reservations'],0)
            self.assertEqual(d['cash_tick'],100000000 if mode=='SHADOW' else 92000000)
            self.assertEqual(d['paper_submissions'],0 if mode=='SHADOW' else 1)
        self.assertEqual(counts,[1,1,1])

    def test_asset_indices_are_bound_before_marks(self):
        # Distinct prices expose accidental YES/NO mark-slot aliasing.
        self.feed[0]['payload']['bids'][0]['price']='0.29'
        self.feed[0]['payload']['asks'][0]['price']='0.30'
        self.feed[1]['payload']['bids'][0]['price']='0.49'
        self.feed[1]['payload']['asks'][0]['price']='0.50'
        self.save(); self.run_native()
        d=self.dashboard()
        self.assertEqual(d['runtime']['cash_tick'],92000000)
        self.assertEqual(d['account']['unrealized_pnl_tick'],-100000)
        self.assertEqual(d['account']['realized_pnl_tick'],0)
        self.run_native(True)
        self.assertEqual(self.dashboard()['account']['unrealized_pnl_tick'],-100000)

    def test_completed_recovery_is_idempotent(self):
        self.save(); self.run_native(); before=self.dashboard()['runtime']; count=self.event_count('EXECUTION_REPORT')
        self.run_native(recover=True); after=self.dashboard()['runtime']
        for key in ['cash_tick','positions','paper_submissions','active_reservations']:
            self.assertEqual(before[key],after[key])
        self.assertEqual(self.event_count('EXECUTION_REPORT'),count)
        self.run_native(code=1)

    def test_unknown_ack_blocks_retry_and_retains_encumbrance(self):
        self.feed[-1]['test_fault']='crash_after_submit_started'
        self.save(); self.run_native(code=86); self.run_native(True,code=2)
        d=self.dashboard()['runtime']
        self.assertEqual(d['lifecycle'],'RECOVERY_BLOCKED')
        self.assertGreater(d['pending_recovery_encumbrance_tick'],0)
        self.assertEqual(self.event_count('SUBMIT_STARTED'),1)
        self.assertEqual(self.event_count('EXECUTION_REPORT'),0)
        self.run_native(True,code=2)
        self.assertEqual(self.event_count('SUBMIT_STARTED'),1)

    def test_crash_after_durable_fill_restores_receipt(self):
        self.feed[-1]['test_fault']='crash_after_first_report'
        self.save(); self.run_native(code=87); self.run_native(True,code=2)
        d=self.dashboard()['runtime']
        self.assertEqual(d['cash_tick'],96000000)
        self.assertEqual(d['pending_durable_reports_restored'],1)
        self.assertEqual(sum(p['qty_lots'] for p in d['positions']),10)
        self.assertEqual(self.event_count('EXECUTION_REPORT'),1)
        self.assertFalse(d['exchange_account_reconciled'])

    def test_duplicate_intent_cannot_resubmit(self):
        self.feed[-1]['test_fault']='duplicate_intent'
        self.save(); self.run_native()
        self.assertEqual(self.dashboard()['runtime']['paper_submissions'],1)
        self.assertEqual(self.event_count('EXECUTION_REPORT'),2)
        self.assertGreater(self.dashboard()['runtime']['duplicate_intents'],0)

    def test_torn_journal_rejected_without_truncation(self):
        self.save(); self.run_native()
        p=Path(self.cfg['output_dir'])/'events.jsonl'
        with p.open('ab') as f: f.write(b'{"torn":')
        before=p.read_bytes(); self.run_native(True,code=1); self.assertEqual(p.read_bytes(),before)

    def test_hash_corruption_rejected(self):
        self.save(); self.run_native()
        p=Path(self.cfg['output_dir'])/'events.jsonl'
        p.write_text(p.read_text().replace('PLAN_PREPARED','PLAN_TAMPERED',1))
        self.run_native(True,code=1)

    def test_frozen_configuration_change_rejected(self):
        self.save(); self.run_native(); self.cfg['config_version']+=1
        self.config.write_text(json.dumps(self.cfg)); self.run_native(True,code=1)

    def test_frozen_feed_change_rejected(self):
        self.save(); self.run_native()
        p=Path(self.cfg['input']); p.write_text(p.read_text().replace('0.40','0.41'))
        self.run_native(True,code=1)

    def test_frozen_artifact_change_rejected(self):
        artifact=self.root/'artifact'; shutil.copytree(self.cfg['oracle_artifact'],artifact)
        self.cfg['oracle_artifact']=str(artifact)
        self.save(); self.run_native()
        with (artifact/'rulebook.json').open('a') as f: f.write('\n')
        self.run_native(True,code=1)

    def test_filled_positions_count_toward_portfolio_limits(self):
        for row in copy.deepcopy(self.feed):
            row['recv_ns']+=10000000
            row['payload']['asks'][0]['price']='0.41'
            self.feed.append(row)
        self.cfg['max_asset_inventory_lots']=10
        self.save(); self.run_native()
        d=self.dashboard()['runtime']
        self.assertEqual(d['paper_submissions'],1)
        self.assertGreater(d['risk_rejections'],0)
        self.assertLessEqual(max(p['qty_lots'] for p in d['positions']),10)

    def test_cash_limit_prevents_overdraft(self):
        self.cfg.update(starting_cash_tick=1000000,max_order_cost_tick=1000000)
        self.save(); self.run_native()
        self.assertGreaterEqual(self.dashboard()['runtime']['cash_tick'],0)

    def test_reconnect_requires_both_new_snapshots(self):
        # No opportunity before disconnect; then only one side is refreshed.
        self.feed[0]['payload']['asks'][0]['price']='0.70'
        self.feed[1]['payload']['asks'][0]['price']='0.50'
        first=copy.deepcopy(self.feed[0]); first.update(recv_ns=1005000000,connection_id=2)
        first['payload']['asks'][0]['price']='0.40'
        self.feed += [{'kind':'disconnect','recv_ns':1002000000},
                      {'kind':'reconnect','recv_ns':1003000000}, first]
        self.save(); self.run_native()
        d=self.dashboard()['runtime']
        self.assertEqual(d['fresh_snapshot_assets'],1)
        self.assertEqual(d['paper_submissions'],0)
        self.assertGreater(d['stopped_intents'],0)

    def test_stale_tick_prevents_new_submission(self):
        self.feed.append({'kind':'tick','recv_ns':3000000000})
        self.save(); self.run_native()
        self.assertEqual(self.dashboard()['runtime']['paper_submissions'],1)
        self.assertGreater(self.dashboard()['runtime']['stale_events'],0)

    def test_decode_failure_clears_snapshot_admission(self):
        self.feed += [{'kind':'packet','recv_ns':1002000000,'payload':'{bad json'},
                      {'kind':'tick','recv_ns':1003000000}]
        self.save(); self.run_native()
        d=self.dashboard()['runtime']
        self.assertEqual(d['decode_errors'],1)
        self.assertEqual(d['fresh_snapshot_assets'],0)
        self.assertEqual(d['paper_submissions'],1)

    def test_stop_is_latched_across_restart(self):
        self.feed.insert(0,{'kind':'stop','recv_ns':999000000})
        self.save(); self.run_native(code=2); self.run_native(True,code=2)
        self.assertEqual(self.dashboard()['runtime']['paper_submissions'],0)

    def test_file_kill_is_latched_across_restart(self):
        self.save(); Path(self.cfg['kill_switch_file']).touch()
        self.run_native(code=2); Path(self.cfg['kill_switch_file']).unlink()
        self.run_native(True,code=2)
        self.assertEqual(self.event_count('SUBMIT_STARTED'),0)

    def test_reconciliation_mismatch_stops_runtime(self):
        self.feed.append({'kind':'account_snapshot','recv_ns':1002000000,'account':{'cash_tick':0}})
        self.save(); self.run_native(code=2)
        self.assertEqual(self.dashboard()['runtime']['reconciliation_mismatches'],1)
        self.run_native(True,code=2)

    def test_live_and_unintegrated_strategies_rejected(self):
        for update in [dict(mode='LIVE'),dict(live_enabled=True),dict(strategy='market_making'),dict(strategy='cross_venue')]:
            baseline=copy.deepcopy(self.cfg); self.cfg.update(update); self.save(); self.run_native(code=1); self.cfg=baseline
        self.assertFalse((self.root/'out/events.jsonl').exists())

    def test_conditional_settlement_artifact_fails_closed(self):
        self.cfg['oracle_artifact']=str(SOURCE/'tests/fixtures/oracle/artifact_positive')
        self.save(); self.run_native(code=1)
        self.assertEqual(self.event_count('SUBMIT_STARTED'),0)

    def test_exclusive_journal_ownership(self):
        self.save(); self.run_native()
        with (Path(self.cfg['output_dir'])/'events.jsonl').open() as f:
            fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.run_native(True,code=1)

    def test_existing_backend_operations_and_read_only(self):
        if not args.backend:
            self.skipTest('backend executable not supplied')
        self.save(); self.run_native()
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
        command=[str(Path(args.backend).resolve()),'--dashboard-file',str(Path(self.cfg['output_dir'])/'dashboard.json'),
                 '--host','127.0.0.1','--port',str(port)]
        proc=subprocess.Popen(command,cwd=SOURCE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        base=f'http://127.0.0.1:{port}'
        try:
            deadline=time.monotonic()+5
            while True:
                try:
                    with urllib.request.urlopen(base+'/api/v1/runtime',timeout=1) as response:
                        runtime=json.load(response)
                    break
                except (OSError,urllib.error.URLError):
                    if time.monotonic()>deadline: raise
                    time.sleep(.02)
            self.assertEqual(runtime['paper_submissions'],1)
            with urllib.request.urlopen(base+'/api/v1/snapshot/latest',timeout=1) as response:
                snapshot=json.load(response)
            self.assertEqual(snapshot['runtime']['cash_tick'],92000000)
            self.assertTrue(snapshot['order_states'])
            with urllib.request.urlopen(base+'/api/v1/markets',timeout=1) as response:
                self.assertEqual(len(json.load(response)['markets']),2)
            request=urllib.request.Request(base+'/api/v1/runtime',data=b'{}',method='POST')
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(request,timeout=1)
            self.assertEqual(error.exception.code,405)
        finally:
            proc.terminate()
            try: proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.communicate(timeout=3)

    def test_sigterm_commits_boundary_and_recovers(self):
        original=copy.deepcopy(self.feed)
        self.feed=[]
        for idx in range(40):
            for row in copy.deepcopy(original):
                row['recv_ns']+=idx*10000000
                self.feed.append(row)
        self.save()
        p=subprocess.Popen([BINARY,'--config',str(self.config)],cwd=SOURCE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        journal=Path(self.cfg['output_dir'])/'events.jsonl'
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            if journal.exists() and 'INPUT_COMMIT' in journal.read_text(): break
            if p.poll() is not None: self.fail('runtime exited before signal injection')
            time.sleep(.002)
        p.send_signal(signal.SIGTERM)
        stdout,stderr=p.communicate(timeout=10)
        self.assertEqual(p.returncode,0,stderr)
        self.assertEqual(self.events()[-1]['type'],'RUNTIME_STOPPED')
        self.run_native(True)
        self.assertEqual(self.dashboard()['runtime']['records'],80)

if __name__ == '__main__':
    unittest.main(argv=[__file__]+remaining,verbosity=2)

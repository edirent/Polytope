#!/usr/bin/env python3
"""V0.2.0 faults use the actual runtime and venue normalizers; no network needed."""
import importlib.util,json,copy,subprocess,hashlib
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('base_runtime',Path(__file__).with_name('runtime_integration.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
COND='0x'+'1'*64
class OperationalContracts(base.RuntimeContracts):
    def venue_fixture(self):
        root=base.SOURCE
        raw=dict(id='111',conditionId=COND,question='Synthetic binary runtime fixture?',description='same-condition synthetic test only',
                 outcomes='["Yes","No"]',clobTokenIds='["101","102"]',active=True,closed=False,acceptingOrders=True,
                 enableOrderBook=True,feesEnabled=False,version='v1',minimumOrderSize=1,orderPriceMinTickSize=.01)
        markets=self.root/'markets.json';markets.write_text(json.dumps([{'venue':'polymarket_international','raw':raw}]))
        oracle_input=self.root/'oracle.jsonl';oracle_input.write_text(json.dumps(dict(market_id=COND,event_id='e',title='fixture',description='fixture',outcomes=['YES','NO'],asset_ids=['101','102'],resolution_source='synthetic',end_time='2030-01-01T00:00:00Z',fetched_at_ns=1,source='fixture'))+'\n')
        rule=json.loads((root/'tests/fixtures/oracle/artifact_positive/rulebook.json').read_text());rule['rules'][0]['variable_ids']=[COND+':YES',COND+':NO']
        rb=self.root/'rules.json';rb.write_text(json.dumps(rule))
        bundle=json.loads((root/'tests/fixtures/runtime/candidate_bundles.json').read_text())
        for n,leg in enumerate(bundle['bundles'][0]['legs']):leg.update(market_id=COND,asset_id=str(101+n))
        bp=self.root/'bundles.json';bp.write_text(json.dumps(bundle))
        subprocess.run([str(root/'build/verify_oracle_workflow'),'--market-snapshot',str(oracle_input),'--rulebook',str(rb),'--candidate-bundles',str(bp),'--out',str(self.root/'oracle')],cwd=root,check=True,stdout=subprocess.DEVNULL)
        self.cfg.update(schema_version=2,mode='PAPER',input_format='venue_jsonl',recorded_fault_fixture=True,
                        markets_file=str(markets),oracle_artifact=str(self.root/'oracle'))
        self.feed=[]
        for n in range(2):
            self.feed.append(self.packet({'event_type':'book','market':COND,'asset_id':str(101+n),'timestamp':'1000',
                     'bids':[{'price':'0.39','size':'10'}],'asks':[{'price':'0.40','size':'10'}]},1000000000+n*1000000))
    def packet(self,payload,stamp,classification='market_data',transport='ws'):
        return dict(kind='venue_packet',venue='polymarket_international',transport=transport,classification=classification,
                    connection_epoch=1,recv_ns=stamp,recv_wall_ns=stamp,raw_payload=json.dumps(payload) if isinstance(payload,dict) else payload)
    def test_plan_and_authorization_precede_adapter(self):
        self.save();self.run_native();events=self.events();types=[e['type'] for e in events]
        self.assertLess(types.index('PLAN_PREPARED'),types.index('SUBMIT_STARTED'))
        self.assertLess(types.index('SUBMIT_STARTED'),types.index('ADAPTER_INVOKED'))
        self.assertLess(types.index('ADAPTER_INVOKED'),types.index('EXECUTION_REPORT'))
        plan=next(e for e in events if e['type']=='PLAN_PREPARED')
        self.assertTrue(plan['authorization']['policy_hash']);self.assertTrue(plan['plan_sha256'])
        for child in plan['orders']:
            for key in ['client_order_id','qty_lots','limit_tick','asset_id','market_id','estimated_vwap_tick','worst_allowed_price_tick']:self.assertIn(key,child)
    def test_crash_pre_submission_has_no_phantom_order(self):
        self.feed[-1]['test_fault']='crash_after_plan_prepared';self.save();self.run_native(code=85);self.run_native(True,code=2)
        d=self.dashboard()['runtime'];self.assertEqual(d['recovery_submission_state'],'NOT_SUBMITTED')
        self.assertEqual(d['pending_recovery_encumbrance_tick'],0);self.assertEqual(self.event_count('ADAPTER_INVOKED'),0)
        self.assertEqual(d['cash_tick'],100000000)
    def test_unknown_ack_has_durable_full_plan(self):
        self.feed[-1]['test_fault']='crash_after_submit_started';self.save();self.run_native(code=86);self.run_native(True,code=2)
        self.assertEqual(self.event_count('PLAN_PREPARED'),1)
        self.assertEqual(self.dashboard()['runtime']['recovery_submission_state'],'ACK_UNKNOWN_QUERY_BEFORE_RETRY')
        self.assertGreater(self.dashboard()['runtime']['pending_recovery_encumbrance_tick'],0)
    def test_disk_full_stops_before_adapter(self):
        self.feed[-1]['test_fault']='disk_full';self.save();self.run_native(code=1)
        self.assertEqual(self.event_count('ADAPTER_INVOKED'),0)
        self.run_native(True,code=2)
    def test_partial_journal_write_is_preserved(self):
        self.feed[-1]['test_fault']='partial_journal_write';self.save();self.run_native(code=1)
        path=Path(self.cfg['output_dir'])/'events.jsonl';before=path.read_bytes();self.run_native(True,code=1);self.assertEqual(before,path.read_bytes())
    def test_strategy_exception_blocks_without_submit(self):
        self.feed[-1]['test_fault']='strategy_exception';self.save();self.run_native(code=1);self.assertEqual(self.event_count('ADAPTER_INVOKED'),0)
        self.run_native(True,code=2)
    def test_live_venue_normalizers_feed_existing_strategy(self):
        self.venue_fixture();self.save();self.run_native();r=self.dashboard()['runtime']
        self.assertEqual(r['paper_submissions'],1);self.assertEqual(r['cash_tick'],92000000)
        self.assertTrue(all(m['admitted'] for m in self.dashboard()['markets']))
        self.run_native(True)
    def test_duplicate_market_frame_does_not_create_trade(self):
        self.venue_fixture();duplicate=copy.deepcopy(self.feed[-1]);duplicate['recv_ns']+=1000000;self.feed.append(duplicate)
        self.save();self.run_native();self.assertEqual(self.event_count('DUPLICATE_MARKET_MESSAGE'),1)
        self.assertEqual(self.dashboard()['runtime']['paper_submissions'],1)
    def test_invalid_snapshot_degrades_book(self):
        self.venue_fixture();raw=json.loads(self.feed[0]['raw_payload']);raw['bids'][0]['price']='0.60';self.feed[0]['raw_payload']=json.dumps(raw)
        self.save();self.run_native();self.assertEqual(self.dashboard()['runtime']['paper_submissions'],0)
        self.assertFalse(all(m['admitted'] for m in self.dashboard()['markets']))
    def test_disconnect_invalidates_and_snapshots_recover(self):
        self.venue_fixture();self.feed.append(self.packet('lost',1002000000,'disconnect','event'))
        for n,row in enumerate(copy.deepcopy(self.feed[:2])):row.update(recv_ns=1003000000+n*1000000,connection_epoch=2);self.feed.append(row)
        self.save();self.run_native();self.assertTrue(all(m['admitted'] for m in self.dashboard()['markets']))
        self.assertFalse(self.dashboard()['runtime']['stop_latched'])
    def test_connection_error_invalidates_before_close_callback(self):
        self.venue_fixture();self.feed.append(self.packet('socket error',1002000000,'connection_error','event'))
        self.save();self.run_native()
        self.assertFalse(any(m['admitted'] for m in self.dashboard()['markets']))
    def test_missing_heartbeat_or_snapshot_age_degrades(self):
        self.venue_fixture();self.feed.append({'kind':'tick','recv_ns':40000000000})
        self.save();self.run_native();self.assertFalse(any(m['admitted'] for m in self.dashboard()['markets']))
        self.assertGreater(self.dashboard()['runtime']['stale_events'],0)
    def test_queue_delay_cannot_refresh_stale_book(self):
        self.venue_fixture()
        for n,row in enumerate(self.feed):
            row['callback_recv_monotonic_ns']=row['recv_ns']
            row['recv_ns']=40000000000+n*1000000
        self.save();self.run_native()
        self.assertEqual(self.dashboard()['runtime']['paper_submissions'],0)
        self.assertFalse(any(m['admitted'] for m in self.dashboard()['markets']))
    def test_invalid_market_status_never_readmits_from_book(self):
        self.venue_fixture();self.feed.append(self.packet({'event_type':'market_resolved','market':COND,'winning_asset_id':'101'},1002000000))
        for n,row in enumerate(copy.deepcopy(self.feed[:2])):row.update(recv_ns=1003000000+n*1000000);self.feed.append(row)
        self.save();self.run_native();self.assertFalse(any(m['admitted'] for m in self.dashboard()['markets']))
    def test_kalshi_sid_sequence_gap_invalidates(self):
        self.venue_fixture()
        markets_path=Path(self.cfg['markets_file']);markets=json.loads(markets_path.read_text())
        ticker='KXTEST-30JAN01-T1'
        markets.insert(0,{'venue':'kalshi','raw':dict(ticker=ticker,event_ticker='KXTEST-30JAN01',title='fixture',status='active',market_type='binary',rules_primary='fixture')})
        markets_path.write_text(json.dumps(markets))
        snapshot={'type':'orderbook_snapshot','sid':7,'seq':1,'msg':{'market_ticker':ticker,'clock':10,'yes_dollars_fp':[['0.39','10']],'no_dollars_fp':[['0.59','10']]}}
        first=self.packet(snapshot,1002000000);first['venue']='kalshi';self.feed.append(first)
        delta={'type':'orderbook_delta','sid':7,'seq':3,'msg':{'market_ticker':ticker,'clock':11,'price_dollars':'0.39','delta_fp':'1','side':'yes'}}
        gap=self.packet(delta,1003000000);gap['venue']='kalshi';self.feed.append(gap)
        self.save();self.run_native()
        books=[m for m in self.dashboard()['markets'] if m['venue']=='kalshi']
        self.assertEqual(len(books),2);self.assertFalse(any(m['admitted'] for m in books))
        counters=self.dashboard()['runtime']['venue_counters']['rejections']
        self.assertGreater(counters.get('SUBSCRIPTION_SEQUENCE_GAP',0),0)
    def test_slow_operations_consumer_does_not_block_trading(self):
        if not base.args.backend:self.skipTest('backend unavailable')
        import socket,time
        self.save()
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
        proc=subprocess.Popen([str(Path(base.args.backend).resolve()),'--dashboard-file',str(Path(self.cfg['output_dir'])/'dashboard.json'),'--port',str(port)],cwd=base.SOURCE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        sock=socket.socket();sock.settimeout(.2)
        try:
            deadline=time.monotonic()+5
            while True:
                try:sock.connect(('127.0.0.1',port));break
                except OSError:
                    if time.monotonic()>deadline:raise
                    time.sleep(.02)
            sock.sendall(b'GET /api/v1/snapshot/latest HTTP/1.1\r\nHost: localhost\r\n')
            self.run_native();self.assertEqual(self.dashboard()['runtime']['paper_submissions'],1)
        finally:
            sock.close();proc.terminate()
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=3)
    def test_real_mode_rejects_unbounded_duration(self):
        self.cfg.update(schema_version=2,mode='PAPER',input_format='jsonl');self.save();self.run_native(code=1)
    def test_fill_receipts_are_unique_and_simulated(self):
        self.save();self.run_native();fills=[e for e in self.events() if e['type']=='EXECUTION_REPORT']
        self.assertEqual(len({e['fill_id'] for e in fills}),2)
        self.assertTrue(all(e['fill_id'].startswith('SIMULATED:') and e['fill_source']=='SIMULATED_PAPER' for e in fills))
if __name__=='__main__':unittest.main(argv=[__file__]+base.remaining,verbosity=2)

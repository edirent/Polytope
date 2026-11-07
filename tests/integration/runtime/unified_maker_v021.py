#!/usr/bin/env python3
"""Faults drive actual unified C++ runtime and canonical modules, never a Python OMS."""
import copy,json,unittest
import runtime_integration as base

class UnifiedMakerContracts(unittest.TestCase):
    save=base.RuntimeContracts.save
    tearDown=base.RuntimeContracts.tearDown
    run_native=base.RuntimeContracts.run_native
    dashboard=base.RuntimeContracts.dashboard
    events=base.RuntimeContracts.events
    event_count=base.RuntimeContracts.event_count
    def setUp(self):
        base.RuntimeContracts.setUp(self)
        for row in self.feed:
            row['payload']['bids'][0].update(price='0.49',size='1000')
            row['payload']['asks'][0].update(price='0.51',size='1000')
        self.cfg.update(schema_version=3,strategy='multi_strategy',max_book_age_ns=15000000000,
            max_order_cost_tick=100000000,max_total_exposure_tick=100000000,max_market_exposure_tick=100000000,max_asset_inventory_lots=1000)
        self.cfg['instruments']=[dict(asset_id=a,asset_index=n,market_id='m1',market_index=0,venue='polymarket_international',account='PAPER',settlement_asset='USDC',tick_size_tick=1) for n,a in enumerate(['asset_yes','asset_no'])]
        self.cfg['maker_execution']=dict(model='conservative',submit_latency_ns=50000000,cancel_latency_ns=100000000,ack_timeout_ns=1000000000,fee_per_lot_tick=0)
        self.cfg['strategies']=[dict(strategy_id=k,strategy_instance_id=i,configuration_epoch=1,priority=n,budget_tick=100000000,model={},
            assets=['asset_yes'],manual_test_requests=k=='market_making',enabled=k!='combinatorial') for n,(k,i) in enumerate([('combinatorial','combo'),('market_making','maker'),('market_making','maker2')])]
        self.time=2000000000
    def control(self,operation,**fields):
        self.feed.append(dict(kind='maker_control',recv_ns=self.time,operation=operation,**fields));self.time+=100000000
        return self.feed[-1]
    def quote(self,**fields):return self.control('quote',**fields)
    def tick(self,advance=100000000):
        self.time+=advance;self.feed.append(dict(kind='tick',recv_ns=self.time));self.time+=100000000
    def report(self,type,**fields):return self.control('report',report_type=type,**fields)
    def run_case(self,code=0):self.save();self.run_native(code=code);return self.dashboard()
    def orders(self):return self.dashboard()['maker_orders']
    def reject_reasons(self):return [e.get('reason','') for e in self.events() if e['type']=='PORTFOLIO_REJECT']
    def seeded_inventory(self):
        self.quote(request_id='seed',quantity=20);self.tick();self.report('FILL',quantity=20,receipt_id='SIMULATED_TEST:seed')
    def test_shared_cash_competition(self):
        self.quote(quantity=150,request_id='a');self.quote(quantity=150,request_id='b',strategy_instance_id='maker2')
        d=self.run_case();self.assertEqual(len(self.orders()),1);self.assertIn('INSUFFICIENT_SHARED_CASH',self.reject_reasons());self.assertEqual(d['runtime']['reserved_cost_tick'],67500000)
    def test_shared_sell_inventory_competition(self):
        self.seeded_inventory();self.quote(request_id='sell1',has_bid=False,has_ask=True,quantity=20);self.quote(request_id='sell2',strategy_instance_id='maker2',has_bid=False,has_ask=True,quantity=20)
        self.run_case();self.assertIn('INSUFFICIENT_SHARED_SELL_INVENTORY',self.reject_reasons())
    def test_submit_then_ack(self):
        self.quote();self.tick();self.run_case();self.assertEqual(self.orders()[0]['status'],'OPEN');self.assertTrue(self.orders()[0]['venue_order_id'].startswith('SIMULATED:'))
    def test_unknown_ack_keeps_cash(self):
        self.quote(test_fault='maker_ack_unknown');self.tick(2000000000);d=self.run_case();self.assertEqual(self.orders()[0]['status'],'ACK_UNKNOWN');self.assertEqual(d['runtime']['reserved_cost_tick'],4500000);self.assertTrue(d['portfolio_frozen'])
    def test_duplicate_quote(self):
        r=self.quote(request_id='duplicate',created_ns=self.time);r2=copy.deepcopy(r);r2['recv_ns']+=1;self.feed.append(r2);self.run_case();self.assertEqual(len(self.orders()),1);self.assertEqual(self.event_count('DUPLICATE_STRATEGY_REQUEST'),1)
    def test_duplicate_ack(self):
        self.quote(request_id='ack');self.tick();self.report('ACK',receipt_id='SIMULATED_TEST:ack1');self.report('ACK',receipt_id='SIMULATED_TEST:ack2');self.run_case();self.assertEqual(self.orders()[0]['status'],'OPEN')
    def test_partial_fill(self):
        self.quote();self.tick();self.report('FILL',quantity=4);d=self.run_case();self.assertEqual(self.orders()[0]['remaining_qty'],6);self.assertEqual(d['runtime']['cash_tick'],98200000);self.assertEqual(d['runtime']['reserved_cost_tick'],2700000)
    def test_duplicate_fill(self):
        self.quote();self.tick();r=self.report('FILL',quantity=4,receipt_id='SIMULATED_TEST:dup');self.feed.append(copy.deepcopy(r));d=self.run_case();self.assertEqual(d['runtime']['cash_tick'],98200000)
    def test_fill_during_cancel_pending(self):
        self.quote();self.tick();self.control('cancel');self.report('FILL',quantity=4);d=self.run_case();self.assertEqual(self.orders()[0]['status'],'CANCEL_PENDING');self.assertEqual(d['runtime']['reserved_cost_tick'],2700000)
    def test_partial_cancel_confirms_remaining_release(self):
        self.quote();self.tick();self.report('FILL',quantity=4);self.control('cancel');self.tick();d=self.run_case();self.assertEqual(self.orders()[0]['status'],'CANCELED');self.assertEqual(d['runtime']['reserved_cost_tick'],0);self.assertEqual(d['runtime']['cash_tick'],98200000)
    def test_cancel_rejected_retains_reservation(self):
        self.quote(test_fault='maker_cancel_reject');self.tick();self.control('cancel');self.tick();d=self.run_case();self.assertEqual(self.orders()[0]['status'],'OPEN');self.assertEqual(d['runtime']['reserved_cost_tick'],4500000)
    def test_cancel_timeout_retains_reservation(self):
        self.quote(test_fault='maker_cancel_timeout');self.tick();self.control('cancel');self.tick();d=self.run_case();self.assertEqual(self.orders()[0]['status'],'CANCEL_PENDING');self.assertEqual(d['runtime']['reserved_cost_tick'],4500000);self.assertTrue(d['portfolio_frozen'])
    def test_cancel_before_ack(self):
        self.quote();self.time=2010000000;self.control('cancel');self.tick();self.run_case();self.assertEqual(self.orders()[0]['status'],'CANCELED')
    def test_replacement_waits_for_unknown_old(self):
        self.quote(request_id='old',test_fault='maker_ack_unknown');self.quote(request_id='new',bid_tick=440000);d=self.run_case();self.assertEqual(len(self.orders()),1);self.assertEqual(d['runtime']['reserved_cost_tick'],4500000);self.assertEqual(self.event_count('MAKER_REPLACE_WAIT'),1)
    def test_overlap_requires_extra_cash(self):
        self.cfg['strategies'][1]['replacement_policy']='OVERLAP_WITH_EXTRA_RESERVATION';self.quote(request_id='old',quantity=150);self.quote(request_id='new',quantity=150,bid_tick=440000)
        self.run_case();self.assertEqual(len(self.orders()),1);self.assertIn('INSUFFICIENT_SHARED_CASH',self.reject_reasons())
    def test_overlap_supported(self):
        self.cfg['strategies'][1]['replacement_policy']='OVERLAP_WITH_EXTRA_RESERVATION';self.quote(request_id='old');self.quote(request_id='new',bid_tick=440000);d=self.run_case();self.assertEqual(len(self.orders()),2);self.assertEqual(d['runtime']['reserved_cost_tick'],8900000)
    def test_post_only_cross_rejected(self):
        self.quote(bid_tick=510000);self.run_case();self.assertFalse(self.orders());self.assertTrue(any('CROSS' in r for r in self.reject_reasons()))
    def test_tick_quantity_rejected(self):
        self.cfg['instruments'][0]['tick_size_tick']=10000;self.quote(bid_tick=455000);self.run_case();self.assertFalse(self.orders())
    def test_stale_quotes_cancel_without_early_release(self):
        self.quote(ttl_ns=60000000000);self.tick();self.tick(20000000000);d=self.run_case();self.assertEqual(self.orders()[0]['status'],'CANCEL_PENDING');self.assertEqual(d['runtime']['reserved_cost_tick'],4500000)
    def test_reconnect_invalidates_active_quote(self):
        self.quote();self.tick();self.feed.append(dict(kind='disconnect',recv_ns=self.time));self.run_case();self.assertEqual(self.orders()[0]['status'],'CANCEL_PENDING')
    def test_strategy_exception_is_contained(self):
        self.cfg['strategies'][1]['manual_test_requests']=False;self.feed[-1]['test_fault']='plugin_exception';d=self.run_case();self.assertEqual(d['strategies'][1]['lifecycle'],'QUARANTINED');self.assertEqual(d['strategies'][2]['lifecycle'],'STOPPED')
    def test_slow_callback_quarantined(self):
        self.cfg['strategies'][1].update(manual_test_requests=False,callback_budget_ns=1000000);self.feed[0]['test_fault']='plugin_slow';d=self.run_case();self.assertEqual(d['strategies'][1]['lifecycle'],'QUARANTINED')
    def test_journal_failure_before_submission(self):
        self.quote(test_fault='disk_full');self.save();self.run_native(code=1);self.assertEqual(self.event_count('MAKER_SUBMIT_STARTED'),0)
    def test_crash_after_plan_not_submitted(self):
        self.quote(test_fault='maker_crash_after_plan');self.save();self.run_native(code=85);self.run_native(True,code=2);d=self.dashboard();self.assertFalse(d['maker_orders']);self.assertEqual(d['runtime']['reserved_cost_tick'],0)
    def test_crash_after_submit_retains_unknown(self):
        self.quote(test_fault='maker_crash_after_submit');self.save();self.run_native(code=86);self.run_native(True,code=2);d=self.dashboard();self.assertEqual(d['maker_orders'][0]['status'],'RECOVERY_BLOCKED');self.assertEqual(d['runtime']['reserved_cost_tick'],4500000)
    def test_crash_after_fill_restores_once(self):
        self.quote();self.tick();self.report('FILL',quantity=4,test_fault='maker_crash_after_fill');self.save();self.run_native(code=87);self.run_native(True,code=2);self.assertEqual(self.dashboard()['runtime']['cash_tick'],98200000);self.run_native(True,code=2);self.assertEqual(self.dashboard()['runtime']['cash_tick'],98200000)
    def test_duplicate_request_ids_across_strategies(self):
        self.quote(request_id='same');self.quote(request_id='same',strategy_instance_id='maker2');self.run_case();self.assertEqual(len(self.orders()),2);self.assertEqual(len({o['client_order_id'] for o in self.orders()}),2)
    def test_limit_reduction_freezes_outstanding(self):
        self.quote();self.control('reduce_limits',max_total_exposure_tick=100);d=self.run_case();self.assertTrue(d['portfolio_frozen']);self.assertEqual(d['runtime']['reserved_cost_tick'],4500000)
    def test_global_kill_during_cancel(self):
        self.quote();self.tick();self.control('cancel');self.feed.append(dict(kind='kill',recv_ns=self.time));d=self.run_case(code=2);self.assertEqual(d['runtime']['reserved_cost_tick'],4500000);self.run_native(True,code=2)
    def test_account_mismatch_freezes(self):
        self.quote();self.feed.append(dict(kind='account_snapshot',recv_ns=self.time,account=dict(cash_tick=0)));d=self.run_case(code=2);self.assertEqual(d['runtime']['reserved_cost_tick'],4500000)
    def test_orphan_reservation_mismatch(self):
        self.control('orphan_reservation');d=self.run_case();self.assertTrue(d['portfolio_frozen'])
    def test_missing_settlement_receipt_does_not_credit_cash(self):
        self.control('settlement_receipt',receipt_id='UNVERIFIED');self.control('settlement_receipt',receipt_id='UNVERIFIED');d=self.run_case();self.assertEqual(d['runtime']['cash_tick'],100000000);self.assertTrue(d['portfolio_frozen'])
    def test_shadow_does_not_invoke_adapter(self):
        self.cfg['replay_execution_policy']='SHADOW';self.quote();self.tick();d=self.run_case();self.assertFalse(d['maker_orders']);self.assertEqual(d['runtime']['reserved_cost_tick'],0);self.assertEqual(self.event_count('MAKER_SUBMIT_STARTED'),0);self.assertEqual(self.event_count('MAKER_ORDER_REPORT'),0)
    def test_live_config_rejected(self):
        self.cfg.update(mode='LIVE',live_enabled=True);self.save();self.run_native(code=1);self.assertFalse((self.root/'out/events.jsonl').exists())
    def test_maker_fees_exact(self):
        self.cfg['maker_execution']['fee_per_lot_tick']=3;self.quote();self.tick();self.report('FILL',quantity=4);d=self.run_case();self.assertEqual(d['runtime']['cash_tick'],98199988);self.assertEqual(d['runtime']['fees_paid_tick'],12);self.assertEqual(d['runtime']['reserved_cost_tick'],2700018)
    def test_completed_restart_deterministic(self):
        self.quote();self.tick();self.report('FILL',quantity=4);self.run_case();before=self.dashboard();self.run_native(True);after=self.dashboard();self.assertEqual(before['maker_orders'],after['maker_orders']);self.assertEqual(before['runtime']['cash_tick'],after['runtime']['cash_tick'])
    def test_duplicate_fill_later_delivery_is_idempotent(self):
        self.quote();self.tick();self.report('FILL',quantity=4,receipt_id='SIMULATED_TEST:late-dup');self.report('FILL',quantity=4,receipt_id='SIMULATED_TEST:late-dup');d=self.run_case();self.assertEqual(d['runtime']['cash_tick'],98200000)
    def test_cancel_then_new_rechecks_after_old_fill(self):
        self.quote(request_id='old');self.tick();self.quote(request_id='new',bid_tick=440000);self.report('FILL',quantity=4);self.tick();d=self.run_case();self.assertEqual(len(self.orders()),2);self.assertEqual(d['runtime']['cash_tick'],98200000);self.assertEqual(d['runtime']['reserved_cost_tick'],4400000)
    def test_duplicate_cancel_request(self):
        self.quote();self.tick();self.control('cancel',request_id='cancel-dup');self.control('cancel',request_id='cancel-dup');self.run_case();self.assertEqual(self.event_count('MAKER_CANCEL_STARTED'),1)
    def test_late_fill_after_terminal_cancel_freezes(self):
        self.quote(request_id='late');self.tick();self.control('cancel');self.tick();self.report('FILL',quantity=4,target_request_id='late');d=self.run_case();self.assertEqual(d['runtime']['cash_tick'],98200000);self.assertTrue(d['portfolio_frozen'])
    def test_fill_before_arrival_rejected(self):
        self.quote();self.time=2000000001;self.report('FILL',quantity=4);self.run_case(code=1);self.assertEqual(self.dashboard()['runtime']['cash_tick'],100000000)
    def test_bad_fill_quantity_keeps_journal_for_review(self):
        self.quote();self.tick();self.report('FILL',quantity=11);self.run_case(code=1);self.run_native(True,code=2);self.assertEqual(self.dashboard()['runtime']['cash_tick'],100000000)
    def test_two_actual_plugins_share_cash(self):
        self.cfg['strategies'][0].update(enabled=True,assets=['asset_yes','asset_no'])
        for r in self.feed:r['payload']['bids'][0].update(price='0.39',size='10');r['payload']['asks'][0].update(price='0.40',size='10')
        q=dict(kind='maker_control',recv_ns=1000500000,operation='quote',request_id='capital',bid_tick=350000,quantity=270)
        self.feed.insert(1,q);d=self.run_case();self.assertEqual(d['runtime']['paper_submissions'],0);self.assertEqual(len(self.orders()),1);self.assertIn('INSUFFICIENT_SHARED_CASH',self.reject_reasons())
    def test_strategy_local_stop_preserves_other_plugin(self):
        self.quote();self.tick();self.control('strategy_stop',strategy_instance_id='maker');self.quote(strategy_instance_id='maker2');d=self.run_case();self.assertEqual(d['strategies'][1]['lifecycle'],'SUSPENDED');self.assertEqual(len(self.orders()),2)
    def test_market_resolution_cancels_quote(self):
        self.quote();self.tick();self.feed.append(dict(kind='packet',recv_ns=self.time,payload=dict(event_type='market_resolved',market='m1',asset_id='asset_yes',winning_asset_id='asset_yes')))
        self.run_case();self.assertEqual(self.orders()[0]['status'],'CANCEL_PENDING')
    def test_shadow_adapter_attempt_rejected_at_gateway_hook(self):
        self.cfg['replay_execution_policy']='SHADOW';self.quote(test_fault='shadow_adapter_attempt');self.run_case(code=1);self.assertEqual(self.event_count('MAKER_SUBMIT_STARTED'),0);self.assertEqual(self.event_count('MAKER_ORDER_REPORT'),0);self.run_native(True,code=2)
    def test_conflicting_receipt_keeps_money_once(self):
        self.quote();self.tick();self.report('FILL',quantity=4,receipt_id='SIMULATED_TEST:conflict');self.report('FILL',quantity=3,receipt_id='SIMULATED_TEST:conflict');d=self.run_case(code=1);self.assertEqual(d['runtime']['cash_tick'],98200000);self.run_native(True,code=2)
    def test_strategy_budget_uses_actual_size(self):
        self.cfg['strategies'][1]['budget_tick']=1000000;self.quote(quantity=150);self.run_case();self.assertIn('STRATEGY_BUDGET_LIMIT',self.reject_reasons());self.assertFalse(self.orders())
    def test_same_condition_event_concentration(self):
        self.cfg['max_market_exposure_tick']=50000000;self.cfg['strategies'][2]['assets']=['asset_no'];self.quote(quantity=100);self.quote(quantity=100,strategy_instance_id='maker2',asset_id='asset_no');self.run_case();self.assertIn('EVENT_CONCENTRATION_LIMIT',self.reject_reasons());self.assertEqual(len(self.orders()),1)
    def test_partial_maker_journal_write_preserved(self):
        self.quote(test_fault='partial_journal_write');self.save();self.run_native(code=1);path=self.root/'out/events.jsonl';before=path.read_bytes();self.run_native(True,code=1);self.assertEqual(before,path.read_bytes())
    def test_old_cancel_duplicate_terminal_has_no_posting(self):
        self.quote(request_id='terminal');self.tick();self.control('cancel');self.tick();self.report('CANCEL_ACK',target_request_id='terminal');d=self.run_case();self.assertEqual(d['runtime']['cash_tick'],100000000);self.assertEqual(d['runtime']['reserved_cost_tick'],0)
    def test_complete_maker_buy_sell_round_trip(self):
        self.seeded_inventory();self.quote(has_bid=False,has_ask=True,quantity=10);self.tick();self.report('FILL',quantity=4);self.report('FILL',quantity=6);d=self.run_case()
        self.assertEqual(d['runtime']['cash_tick'],96500000);self.assertEqual(d['account']['realized_pnl_tick'],1000000)
        self.assertEqual(next(p for p in d['runtime']['positions'] if p['asset_id']=='asset_yes')['qty_lots'],10);self.run_native(True)
    def test_cancel_rejection_before_first_ack_freezes_globally(self):
        self.quote(test_fault='maker_ack_unknown');self.control('cancel');self.report('CANCEL_REJECT');self.quote(strategy_instance_id='maker2',request_id='new-risk')
        d=self.run_case();self.assertTrue(d['portfolio_frozen']);self.assertEqual(len(self.orders()),1);self.assertEqual(d['runtime']['reserved_cost_tick'],4500000)

if __name__=='__main__':unittest.main(argv=[__file__]+base.remaining,verbosity=2)

import importlib.util
import sys
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'research'))
import pmxt_shock_fixed_tests as suite


class FixedShockTest(unittest.TestCase):
    def test_strategy_stays_fixed_across_stresses(self):
        m={'condition_id':'condition','up_token':'up','down_token':'down','start_ms':100000,'expiry_ms':1000000}
        baseline=suite.configuration(m,'baseline')
        for profile in suite.PROFILES:
            cfg=suite.configuration(m,profile)
            self.assertEqual(cfg['candidate'],'shock_entry')
            for key in ('quote','regime','accounting','market','terminal_reduce_request_ms','decision_interval_ms'):
                self.assertEqual(cfg[key],baseline[key])
        self.assertEqual(suite.configuration(m,'btc250')['btc_receive_delay_ms'],250)
        self.assertEqual(suite.configuration(m,'joint')['latency_assumptions_ms'],
            {'submit':100,'cancel':200,'fill_notice':200,'terminal_reduce':275})

    def test_settlement_inventory_and_null_markout_are_not_free_profit(self):
        # Sell five winning shares at 0.51: cash rises 2.55 but settlement loses five.
        row={'settlement_pnl_tick':-2450000,'start_ms':1777593600000,
             'markout_quantity':[0,0,0],'quantity_weighted_markout':[None,None,None],
             'submitted':1,'fills':1,'cancel_pending_fill_lots':5,'post_expiry_fill_lots':0,
             'cash_tick':102550000,'up_lots':20,'down_lots':25,'winner':'Up'}
        result=suite.summarize([row])
        self.assertAlmostEqual(result['pnl_usd'],-2.45)
        self.assertAlmostEqual(result['gross_cash_change_usd'],2.55)
        self.assertEqual(result['settlement_inventory_change_usd'],-5)
        self.assertEqual(result['weighted_markout_100ms_1s_5s'],[None,None,None])
        self.assertEqual(result['chronological_independent_contract_pnl_max_drawdown_usd'],2.45)


if __name__=='__main__':unittest.main()

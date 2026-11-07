import sys
import unittest
from pathlib import Path
import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'research'))
import pmxt_shock_diagnostics as d
import pmxt_shock_regime_comparison as comparison


def fill(channel,price,quantity=5):
    return {'channel':channel,'price_tick':round(price*1e6),'quantity_lots':quantity,'terminal':False}


class AccountingTest(unittest.TestCase):
    def test_closed_complementary_pair_has_outcome_independent_profit(self):
        fills=[fill(0,.43),fill(2,.54)]
        for winner in ['Up','Down']:
            r=d.decompose(fills,winner)
            self.assertEqual(r['closed_paired_tick'],150000)
            self.assertEqual(r['terminal_directional_tick'],0)
            self.assertEqual(r['categories_tick']['buy_pair'],150000)

    def test_sell_initial_pair_is_not_free_cash(self):
        r=d.decompose([fill(1,.61),fill(3,.42)],'Up')
        self.assertEqual(r['total_tick'],150000)
        self.assertEqual(r['categories_tick']['sell_pair'],150000)
        self.assertEqual(r['terminal_directional_tick'],0)

    def test_unpaired_initial_winning_token_sale_is_a_directional_loss(self):
        r=d.decompose([fill(1,.88)],'Up')
        self.assertEqual(r['closed_paired_tick'],0)
        self.assertEqual(r['terminal_directional_tick'],-600000)
        self.assertEqual(r['fill_cash_change_tick'],4400000)

    def test_matching_policy_changes_attribution_but_not_total(self):
        fills=[fill(0,.4),fill(0,.7),fill(1,.6)]
        fifo=d.decompose(fills,'Up');lifo=d.decompose(fills,'Up','lifo');avg=d.average_decompose(fills,'Up')
        self.assertEqual(fifo['closed_paired_tick'],1000000)
        self.assertEqual(lifo['closed_paired_tick'],-500000)
        self.assertAlmostEqual(avg['closed_paired_tick'],250000)
        for r in [fifo,lifo,avg]:
            self.assertAlmostEqual(r['closed_paired_tick']+r['terminal_directional_tick'],2500000)

    def test_flip_and_complementary_channels_reconcile_every_prefix(self):
        rng=np.random.default_rng(123)
        fills=[fill(int(rng.integers(4)),float(rng.integers(1,99))/100,int(rng.integers(1,8))) for _ in range(80)]
        for i in range(1,len(fills)+1):
            for winner in ['Up','Down']:
                a=d.decompose(fills[:i],winner);b=d.decompose(fills[:i],winner,'lifo');c=d.average_decompose(fills[:i],winner)
                self.assertEqual(a['total_tick'],b['total_tick'])
                self.assertAlmostEqual(c['closed_paired_tick']+c['terminal_directional_tick'],a['total_tick'],places=6)

    def test_missing_predecessor_is_not_a_shock_crossing(self):
        n=500;times=np.arange(n)*50;ready=np.ones(n,dtype=bool);ready[:2]=False
        alpha=np.zeros(n);alpha[2:5]=.02;alpha[10:12]=.02;alpha[20:23]=.02;alpha[120:123]=-.02
        rec,audit=d.episodes(times,ready,alpha,np.ones(n)*.5,np.ones(n)*10,np.full(n,2))
        self.assertEqual(audit['raw_crossings'],3)
        self.assertEqual([r['now_ms'] for r in rec],[500,6000])

    def test_boolean_persistence_statistics_keep_missing_separate(self):
        r=d.describe([True,True,False])
        self.assertEqual(r['n'],3)
        self.assertAlmostEqual(r['mean'],2/3)
        self.assertEqual(d.describe([]),{'n':0})

    def test_day_block_difference_preserves_weighted_means(self):
        old=[(10,5),(20,10)];new=[(6,2),(30,10)]
        r=comparison.difference_interval(old,new)
        self.assertEqual(r['new_minus_old'],1)
        self.assertEqual(r['day_block_bootstrap_95pct'],[1,1])

    def test_state_standardization_excludes_halt_and_keeps_signal_direction(self):
        def record(alpha,eligible):
            return {'alpha':alpha,'eligible_shock':eligible,'onset_values':[abs(alpha),1,100,1,1,.02,.1]}
        r=comparison.standardize([record(.03,True),record(-.03,True),record(.05,False)],[.5,1.5,2])
        self.assertEqual(sum(map(len,r.values())),2)
        self.assertEqual(len(r),2)


if __name__=='__main__':unittest.main()

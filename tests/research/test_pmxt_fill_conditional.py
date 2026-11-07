import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'research'))
import pmxt_fill_conditional as study


def order(id=1,start=900,end=1100,fill=False):
    return {'id':id,'active_ms':start,'active_sequence':10,'end_ms':end,'end_sequence':30,
            'ended_by_fill':fill,'created_ms':start-25}


def shock(ms=1000,seq=20):return {'now_ms':ms,'sequence':seq,'alpha':.02}


def fill(ms=1050,seq=25,id=1,mo=None):
    return {'event_ms':ms,'sequence':seq,'order_id':id,'quantity_lots':5,'channel':0,
            'markout':[None,None,None] if mo is None else mo,'visible_spread_tick':10000,'cancel_pending':False}


class ActualFillStudyTest(unittest.TestCase):
    def test_unfilled_actual_order_is_in_probability_denominator_only(self):
        rows,_=study.aggregate([order(),order(id=2)],[shock()],[fill(mo=[.01,.02,.03])])
        out=study.combined([{'bins':rows}]);b=out[1]
        self.assertEqual(b['at_risk_order_bucket_samples'],2)
        self.assertEqual(b['filled_order_bucket_samples'],1)
        self.assertEqual(b['fill_probability'],.5)
        self.assertEqual(b['markout_cents'],[1,2,3])
        self.assertEqual(b['spread_cents'],1)

    def test_source_fill_before_same_ms_observed_shock_is_pre(self):
        rows,coverage=study.aggregate([order(end=1000,fill=True)],[shock()],[fill(ms=1000,seq=15)])
        self.assertEqual(rows[0]['fill_events'],1)
        self.assertEqual(rows[1]['fill_events'],0)
        self.assertEqual(coverage['same_timestamp_pre_shock_fills'],1)

    def test_exact_100ms_boundary_and_closing_fill_remain_in_risk_set(self):
        rows,_=study.aggregate([order(end=1100,fill=True)],[shock()],[fill(ms=1100)])
        self.assertEqual(rows[1]['fill_events'],0)
        self.assertEqual(rows[2]['fill_events'],1)
        self.assertEqual(rows[2]['at_risk_order_bucket_samples'],1)

    def test_new_shock_resets_age_and_no_fill_is_duplicated(self):
        rows,_=study.aggregate([order(end=3000)],[shock(),shock(ms=1500,seq=21)],[fill(ms=1550)])
        self.assertEqual(rows[1]['fill_events'],1)
        self.assertEqual(sum(r['fill_events'] for r in rows),1)
        self.assertEqual(rows[0]['at_risk_order_bucket_samples'],1)

    def test_missing_future_markout_is_not_zero_filled(self):
        rows,_=study.aggregate([order()],[shock()],[fill()])
        b=study.combined([{'bins':rows}])[1]
        self.assertEqual(b['markout_cents'],[None,None,None])
        self.assertEqual(b['fill_lots'],5)

    def test_partial_fills_count_once_for_probability(self):
        rows,_=study.aggregate([order()],[shock()],[fill(ms=1020),fill(ms=1050)])
        self.assertEqual(rows[1]['filled_order_bucket_samples'],1)
        self.assertEqual(rows[1]['fill_events'],2)
        self.assertEqual(rows[1]['fill_lots'],10)

    def test_outside_event_window_is_not_moved_into_pre(self):
        rows,a=study.aggregate([order(start=6500,end=7000)],[shock()],[fill(ms=6600)])
        self.assertEqual(sum(r['fill_events'] for r in rows),0)
        self.assertEqual(a['excluded_fill_events'],1)


if __name__=='__main__':unittest.main()

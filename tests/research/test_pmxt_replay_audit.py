import datetime as dt
from decimal import Decimal
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'research'))
from pmxt_replay_audit import (Depth, Quote, QueueBranch, Timeline, arrival_check,
                               label, millis, order_label, timelines)


class ReplayAuditTest(unittest.TestCase):
    def test_token_buy_sell_and_identity(self):
        t = Timeline([Quote(990, 985, 990, 400000, 420000, 0),
                      Quote(1095, 1090, 1095, 430000, 450000, 1)])
        a = label(t, at=1000, p=400000, side='BUY', quantity=Decimal('2.5'), expiry=10000)
        b = label(t, at=1000, p=420000, side='SELL', quantity=1, expiry=10000)
        self.assertAlmostEqual(a['S_usd_per_share'], .01)
        self.assertAlmostEqual(a['horizons']['100']['A_usd_per_share'], .03)
        self.assertAlmostEqual(a['horizons']['100']['E_usd_per_share'], .04)
        self.assertAlmostEqual(a['horizons']['100']['partial_D_usd'], .10)
        self.assertAlmostEqual(b['horizons']['100']['E_usd_per_share'], -.02)

    def test_strict_pre_and_asof_not_first_after(self):
        t = Timeline([Quote(990, 990, 990, 400000, 420000, 0),
                      Quote(1000, 1000, 1000, 500000, 520000, 1),
                      Quote(1101, 1101, 1101, 800000, 820000, 2)])
        a = label(t, at=1000, p=400000, side='BUY', quantity=1, expiry=10000)
        self.assertEqual(a['pre_quote']['row'], 0)
        self.assertEqual(a['horizons']['100']['future_quote']['row'], 1)
        self.assertEqual(a['horizons']['100']['quote_age_ms'], 100)
        self.assertAlmostEqual(a['horizons']['100']['E_usd_per_share'], .11)

    def test_missing_close_invalid_and_stale_not_zero(self):
        t = Timeline([Quote(990, 990, 990, 400000, 420000, 0)])
        a = label(t, at=1000, p=400000, side='BUY', quantity=1, expiry=2000)
        self.assertIsNone(a['horizons']['100']['E_usd_per_share'])
        self.assertEqual(a['horizons']['100']['invalid_reason'], 'stale_quote')
        self.assertEqual(a['horizons']['100']['quote_age_ms'], 110)
        self.assertEqual(a['horizons']['1000']['invalid_reason'], 'market_closed')
        t = Timeline([Quote(900, 900, 900, 400000, 420000, 0),
                      Quote(999, 999, 999, 500000, 490000, 1)])
        self.assertEqual(t.lookup(1000, 100)[1], 'invalid_touch')

    def test_receive_age_does_not_hide_source_age(self):
        t = Timeline([Quote(990, 700, 990, 400000, 420000, 0)])
        self.assertEqual(t.lookup(1000, 100)[1], 'stale_quote')
        a = label(t, at=1000, p=400000, side='BUY', quantity=1, expiry=10000)
        self.assertEqual(a['pre_age_ms'], 10)
        self.assertEqual(a['pre_source_age_ms'], 300)
        self.assertIsNone(a['S_usd_per_share'])

    def test_post_only_on_arrival_and_unknown_queue(self):
        d = Depth(bid=450000, ask=460000, source=1000, received=1010)
        self.assertEqual(arrival_check(d, 'BUY', 460000, 1025)['status'], 'post_only_rejected')
        self.assertEqual(arrival_check(d, 'SELL', 450000, 1025)['status'], 'post_only_rejected')
        self.assertEqual(arrival_check(d, 'BUY', 450000, 1025)['status'], 'queue_unknown')
        d.known.add(('BUY', 450000))
        d.levels['BUY'][450000] = Decimal('7.125')
        self.assertEqual(arrival_check(d, 'BUY', 450000, 1025)['queue_ahead'], '7.125')

    def test_partial_fill_pending_cancel_and_fractional_volume(self):
        q = QueueBranch(Decimal(5), Decimal('2.5'), 1000, 1200)
        self.assertEqual(q.trade(999, 100), 0)
        self.assertEqual(q.trade(1050, '3.25'), Decimal('.75'))
        self.assertEqual(q.trade(1199, '2.5'), Decimal('2.5'))
        self.assertEqual(q.quantity, Decimal('1.75'))
        self.assertEqual(q.trade(1200, 5, before_cancel=False), 0)
        self.assertEqual(q.trade(1201, 5), 0)

    def test_queue_scenarios_no_double_count_no_new_queue_ahead(self):
        for scenario, expected in [('conservative', Decimal(5)), ('optimistic', Decimal(2))]:
            q = QueueBranch(Decimal(5), Decimal(10), 0, 1000, scenario)
            q.trade(100, 5)
            q.book_change(10, 5)  # Could be the same trade: never count twice.
            self.assertEqual(q.ahead, 5)
            q.book_change(5, 20)  # New orders do not jump ahead.
            self.assertEqual(q.ahead, 5)
            q.attributed_cancel(3)  # Only separate evidence can call this.
            self.assertEqual(q.ahead, expected)

    def test_order_unknown_and_partial_missing(self):
        self.assertIsNone(order_label([], 100, complete_observation=False))
        self.assertEqual(order_label([], 100, complete_observation=True), 0)
        self.assertIsNone(order_label([{'horizons': {'100': {'partial_D_usd': None}}}],
                                      100, complete_observation=True))

    def test_integer_timestamp_exact(self):
        timestamp = dt.datetime(2026, 5, 1, 0, 0, 0, 565000, tzinfo=dt.timezone.utc)
        self.assertEqual(millis(timestamp), 1777593600565)

    def test_same_ms_tie_sensitivity_and_no_late_source_regression(self):
        rows = []
        for i, (src, recv, bid) in enumerate([(100, 100, '.4'), (100, 100, '.5'), (90, 110, '.3')]):
            rows.append({'raw_id': i, 'src': src, 'recv': recv, 'asset_id': 'U',
                         'event_type': 'price_change', 'side': 'BUY', 'price': bid,
                         'size': '1', 'best_bid': bid, 'best_ask': '.6'})
        a = timelines(rows, {'U': 0}, 'received')[0]
        b = timelines(rows, {'U': 0}, 'received', True)[0]
        self.assertEqual(a.lookup(110, 100)[0].bid, 500000)
        self.assertEqual(b.lookup(110, 100)[0].bid, 400000)


if __name__ == '__main__':
    unittest.main()

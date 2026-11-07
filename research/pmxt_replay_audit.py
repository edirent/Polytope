#!/usr/bin/env python3
"""Stage-1 audit of legacy fills against raw PMXT events; never produces new PnL.

The label and arrival primitives are deliberately separate from the frozen C++
strategy. Observed public events do not establish counterfactual FIFO truth.
"""
from __future__ import annotations

import argparse
import bisect
from collections import Counter
from dataclasses import dataclass, field
import datetime as dt
from decimal import Decimal
import gzip
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SUITE = ROOT / 'runs/pmxt_shock_fixed_tests_20261006'
OUT = ROOT / 'runs/pmxt_reentry_audit_v3'
DATA = {'reused': ROOT / 'runs/pmxt_btc15m_20260417_20260430',
        'confirmation': ROOT / 'runs/pmxt_shock_fixed_20260501_20260507'}
HORIZONS = (100, 1000, 5000)
ONE = 1_000_000


def sha(path):
    with Path(path).open('rb') as f:
        h = hashlib.file_digest(f, 'sha256')
    return h.hexdigest()


def write(path, data):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False) + '\n')


def price(value):
    return int(Decimal(str(value)) * ONE)


def millis(value):
    delta = value - dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
    return delta.days * 86_400_000 + delta.seconds * 1000 + delta.microseconds // 1000


@dataclass(frozen=True)
class Quote:
    time: int
    source: int
    received: int
    bid: int
    ask: int
    row: int

    @property
    def mid(self):
        return (self.bid + self.ask) / (2 * ONE)


class Timeline:
    def __init__(self, quotes):
        self.quotes = quotes
        self.times = [q.time for q in quotes]

    def candidate(self, at, *, strict=False):
        i = (bisect.bisect_left if strict else bisect.bisect_right)(self.times, at) - 1
        return self.quotes[i] if i >= 0 else None

    def lookup(self, at, max_age, *, strict=False, expiry=None):
        """Last *state*, not last healthy quote: invalid updates invalidate labels."""
        if expiry is not None and at >= expiry:
            return None, 'market_closed'
        i = (bisect.bisect_left if strict else bisect.bisect_right)(self.times, at) - 1
        if i < 0:
            return None, 'missing_quote'
        q = self.quotes[i]
        if not 0 < q.bid < q.ask < ONE:
            return None, 'invalid_touch'
        if at - q.time > max_age or at - q.source > max_age:
            return None, 'stale_quote'
        if q.source > at or (q.received > at and q.time == q.received):
            raise ValueError('future information in quote timeline')
        return q, None


def label(timeline, *, at, p, side, quantity, expiry, max_age=100):
    """Primary clock is exporter received, strictly before fill at source time.

    Both endpoints use the same chosen clock. No first-after interpolation and
    no terminal/unknown zero labels. This is exporter visibility, not our client.
    """
    if side not in ('BUY', 'SELL') or quantity <= 0:
        raise ValueError('invalid fill')
    d = 1 if side == 'BUY' else -1
    pre, why = timeline.lookup(at, max_age, strict=True, expiry=expiry)
    pre_candidate = timeline.candidate(at, strict=True)
    spread_edge = d * (pre.mid - p / ONE) if pre else None
    result = {'S_usd_per_share': spread_edge, 'S_cents_per_share': None if pre is None else 100 * spread_edge,
              'pre_quote': None if pre_candidate is None else vars(pre_candidate), 'pre_invalid_reason': why,
              'pre_age_ms': None if pre_candidate is None else at - pre_candidate.time,
              'pre_source_age_ms': None if pre_candidate is None else at - pre_candidate.source, 'horizons': {}}
    for h in HORIZONS:
        future, reason = timeline.lookup(at + h, max_age, expiry=expiry)
        candidate = timeline.candidate(at + h)
        valid = pre is not None and future is not None
        a = d * (future.mid - pre.mid) if valid else None
        e = d * (future.mid - p / ONE) if valid else None
        if valid and abs(e - spread_edge - a) > 1e-12:
            raise AssertionError('S + A != E')
        result['horizons'][str(h)] = {
            'valid': valid, 'invalid_reason': why if pre is None else reason,
            'future_quote': None if candidate is None else vars(candidate),
            'quote_age_ms': None if candidate is None else at + h - candidate.time,
            'source_age_ms': None if candidate is None else at + h - candidate.source,
            'A_usd_per_share': a, 'E_usd_per_share': e,
            'A_cents_per_share': None if a is None else 100 * a,
            'E_cents_per_share': None if e is None else 100 * e,
            'partial_D_usd': None if e is None else float(quantity) * e}
    return result


@dataclass
class Depth:
    levels: dict = field(default_factory=lambda: {'BUY': {}, 'SELL': {}})
    full: bool = False
    known: set = field(default_factory=set)
    bid: int = 0
    ask: int = ONE
    source: int = -1
    received: int = -1
    tick: int | None = None  # No backward use of end-of-market Gamma tick.

    def apply(self, row):
        kind = row['event_type']
        if kind == 'tick_size_change':
            if row['new_tick_size'] is not None:
                self.tick = price(row['new_tick_size'])
            return
        if kind not in ('book', 'price_change') or row['src'] < self.source:
            return
        # An hour with no token update is an uncertainty reset, not proof of loss.
        if self.received >= 0 and row['recv'] - self.received > 3_600_000:
            self.levels = {'BUY': {}, 'SELL': {}}
            self.known.clear()
            self.full = False
        if kind == 'book':
            for side, col in [('BUY', 'bids'), ('SELL', 'asks')]:
                self.levels[side] = {price(p): Decimal(str(q)) for p, q in json.loads(row[col] or '[]') if Decimal(str(q)) > 0}
            self.full = True
            self.bid = max(self.levels['BUY'], default=0)
            self.ask = min(self.levels['SELL'], default=ONE)
        else:
            if row['side'] in ('BUY', 'SELL') and row['price'] is not None and row['size'] is not None:
                side, p, q = row['side'], price(row['price']), Decimal(str(row['size']))
                self.known.add((side, p))
                if q > 0:
                    self.levels[side][p] = q
                else:
                    self.levels[side].pop(p, None)
            # Explicit NBBO is valid price evidence even when depth is partial.
            self.bid = price(row['best_bid']) if row['best_bid'] is not None else 0
            self.ask = price(row['best_ask']) if row['best_ask'] is not None else ONE
            for p in list(self.levels['BUY']):
                if p > self.bid:
                    del self.levels['BUY'][p]
            for p in list(self.levels['SELL']):
                if p < self.ask:
                    del self.levels['SELL'][p]
        self.source, self.received = row['src'], row['recv']

    def queue(self, side, p):
        if self.full or (side, p) in self.known:
            return self.levels[side].get(p, Decimal(0))
        return None


def arrival_check(depth, side, p, at, max_age=1500):
    if at - depth.source > max_age or not 0 < depth.bid < depth.ask < ONE:
        return {'status': 'unknown_arrival_book', 'queue_ahead': None}
    if (side == 'BUY' and p >= depth.ask) or (side == 'SELL' and p <= depth.bid):
        return {'status': 'post_only_rejected', 'queue_ahead': None}
    q = depth.queue(side, p)
    return {'status': 'queue_unknown' if q is None else 'passive_arrival',
            'queue_ahead': None if q is None else str(q)}


@dataclass
class QueueBranch:
    """One independent same-token branch; L2 ambiguity never manufactures fill.

    A cancellation must be independently attributed, excluding any trade volume
    already consumed. Raw book decreases alone never call attributed_cancel.
    """
    quantity: Decimal
    ahead: Decimal
    arrival_ms: int
    cancel_effective_ms: int
    scenario: str = 'conservative'
    fills: list = field(default_factory=list)
    unknown_shrink: Decimal = Decimal(0)

    def __post_init__(self):
        if self.scenario not in ('conservative', 'optimistic') or min(self.quantity, self.ahead) < 0:
            raise ValueError('invalid queue branch')

    def trade(self, at, quantity, *, after_arrival=True, before_cancel=True):
        if at < self.arrival_ms or at > self.cancel_effective_ms or not after_arrival or not before_cancel:
            return Decimal(0)
        volume = Decimal(str(quantity))
        if volume < 0:
            raise ValueError('negative volume')
        consumed = min(self.ahead, volume)
        self.ahead -= consumed
        filled = min(self.quantity, volume - consumed)
        self.quantity -= filled
        if filled:
            self.fills.append((at, filled))
        return filled

    def attributed_cancel(self, quantity):
        if self.scenario == 'optimistic':
            self.ahead = max(Decimal(0), self.ahead - Decimal(str(quantity)))

    def book_change(self, old, new):
        self.unknown_shrink += max(Decimal(0), Decimal(str(old)) - Decimal(str(new)))
        # Neither a decrease nor an increase changes FIFO position without attribution.


def order_label(fills, horizon, *, complete_observation):
    if not complete_observation:
        return None
    if not fills:
        return 0.0
    marks = [f['horizons'][str(horizon)]['partial_D_usd'] for f in fills]
    return sum(marks) if all(m is not None for m in marks) else None


def read_raw(paths):
    import pyarrow.parquet as pq
    rows = []
    schemas = set()
    for path in paths:
        table = pq.read_table(path)
        schemas.add(str(table.schema))
        for row in table.to_pylist():
            row['src'] = millis(row['timestamp'])
            row['recv'] = millis(row['timestamp_received'])
            row['raw_id'] = len(rows)
            rows.append(row)
    return rows, schemas


def timelines(rows, tokens, clock, reverse_ties=False):
    states = [Depth(), Depth()]
    quotes = [[], []]
    key = 'src' if clock == 'source' else 'recv'
    ordered = sorted(rows, key=lambda r: (r[key], -r['raw_id'] if reverse_ties else r['raw_id']))
    for row in ordered:
        if row['recv'] < row['src'] or row['asset_id'] not in tokens:
            continue
        ti = tokens[row['asset_id']]
        prev = states[ti].source
        states[ti].apply(row)
        if row['event_type'] in ('book', 'price_change') and row['src'] >= prev:
            d = states[ti]
            quotes[ti].append(Quote(row[key], d.source, d.received, d.bid, d.ask, row['raw_id']))
    return [Timeline(q) for q in quotes]


def arrival_probes(rows, tokens, requests, *, inclusive=True):
    states = [Depth(), Depth()]
    ordered = sorted((r for r in rows if r['recv'] >= r['src']), key=lambda r: (r['src'], r['raw_id']))
    cursor = 0
    result = {}
    for at, ti, side, p, ident in sorted(requests):
        while cursor < len(ordered) and (ordered[cursor]['src'] <= at if inclusive else ordered[cursor]['src'] < at):
            row = ordered[cursor]
            if row['asset_id'] in tokens:
                states[tokens[row['asset_id']]].apply(row)
            cursor += 1
        d = states[ti]
        result[ident] = {**arrival_check(d, side, p, at), 'source_age_ms': at - d.source,
                         'bid_tick': d.bid, 'ask_tick': d.ask, 'historical_tick': d.tick,
                         'tick_history_status': 'unknown' if d.tick is None else 'observed_change',
                         'queue_snapshot_full': d.full}
    return result


def choose_pilots():
    def cases(window, day=None):
        result = []
        for p in sorted((SUITE / window / 'baseline').glob('*/summary.json')):
            case = json.loads(p.read_text())
            case['day_utc'] = dt.datetime.fromtimestamp(case['start_ms'] / 1000, dt.timezone.utc).strftime('%Y-%m-%d')
            if case['fills'] and (day is None or case['day_utc'] == day):
                result.append(case)
        return result
    # Existing outcome-informed DEVELOPMENT fixtures, never holdout selection.
    old, new = cases('reused'), cases('confirmation')
    picks = [('reused', old[0]), ('confirmation', new[0])]
    for day in ('2026-05-03', '2026-05-04', '2026-05-07'):
        group = [c for c in new if c['day_utc'] == day]
        picks.append(('confirmation', max(group, key=lambda c: (c['fills'], c['slug']))))
    return [{'legacy_window_key': w, 'classification': 'development', 'slug': c['slug']} for w, c in picks]


def freeze():
    source = ROOT / 'research/pmxt_replay_audit.py'
    request = Path('/home/edirent/.codex/attachments/cbd37cc4-7015-469f-a16c-f242af1dc545/Pasted text.txt')
    frozen = json.loads((SUITE / 'frozen_manifest.json').read_text())
    configuration = {
        'version': 'reentry_audit_v3', 'stage': 'audit_only', 'bulk_shadow_allowed': False,
        'supersedes': {'v1': 'Incomplete: binary market-ID trace serialization failed.',
                      'v2': 'Incomplete: missing legacy trace. v3 replays frozen binary, retains invalid quote ages, optimizes tie audit. No thresholds changed.'},
        'original_strategy': frozen['strategy_config'], 'original_model_sha256': frozen['model_sha256'],
        'original_executable_sha256': frozen['executable_sha256'],
        'audit_source_sha256': sha(source), 'taskbook_sha256': sha(request),
        'data_classification': {'all_used_April_May_windows': 'development',
                                'train': None, 'validation': None, 'test': None,
                                'reason': 'No value-model split or untouched test until audit passes.'},
        'pilot_selection': 'first filled reused/new market, and highest fill-count markets on May 3/4/7; development diagnostics only',
        'pilots': choose_pilots(),
        'label': {'horizons_ms': list(HORIZONS), 'max_quote_age_ms': 100,
                  'primary': 'raw token midpoint, exporter receive clock, strict pre-fill; asof future',
                  'sensitivity': 'source clock endpoints and reversed same-ms raw order',
                  'units': 'USD/share and cents/share; D in USD', 'unknown_value': None},
        'latency': {'submit': {'ms': 25, 'source': 'assumed'},
                    'cancel': {'ms': 100, 'source': 'assumed'},
                    'notice': {'ms': 50, 'source': 'assumed'},
                    'BTC_receive': {'ms': 20, 'source': 'assumed'},
                    'PM_receive': {'source': 'measured_exporter_not_our_client'}},
        'future_shadow_spec_pending_audit': {
            'grid_ms': 100, 'execution': 'raw events', 'Q0': 5, 'client_lifetime_ms': 1000,
            'candidate_prices': ['token best bid', 'token best bid minus historical tick',
                                 'token best ask', 'token best ask plus historical tick'],
            'tokens': ['UP', 'DOWN'], 'WAIT': True,
            'queue_scenarios': ['conservative', 'optimistic_attributable_cancellations_only'],
            'cancel_policy': 'frozen engine rules; arrival rejection and TTL explicit',
            'exit_fees_inventory_policy': 'not yet audited/frozen; BLOCK model and PnL training'},
        'fee_policy': 'No rebate added; historical exit fee schedule requires provenance verification',
        'gate': ['raw execution and lifecycle integrated and traced', 'historical initial tick verified',
                 'ambiguous ordering sensitivity', 'coverage-qualified no-fill labels',
                 'exit/fee/inventory policy audited and frozen']}
    if (OUT / 'manifest.json').exists():
        existing = json.loads((OUT / 'manifest.json').read_text())
        if existing != configuration:
            raise ValueError('v1 frozen: changed code/config needs a new version/output directory')
    else:
        write(OUT / 'manifest.json', configuration)
        (OUT / 'taskbook.txt').write_bytes(request.read_bytes())
    return configuration


def audit_case(pilot, config):
    window, slug = pilot['legacy_window_key'], pilot['slug']
    data = DATA[window]
    market = next(m for m in json.loads((data / 'markets.json').read_text()) if m['slug'] == slug)
    tokens = {market['up_token']: 0, market['down_token']: 1}
    paths = sorted((data / 'market_rows' / slug).glob('*.parquet'))
    rows, schemas = read_raw(paths)
    active = [r for r in rows if market['start_ms'] <= r['src'] < market['expiry_ms']]
    legacy_dir = SUITE / window / 'baseline' / slug
    dest = OUT / slug
    dest.mkdir(parents=True, exist_ok=True)
    legacy_trace = dest / 'legacy_trace.jsonl'
    old_summary = json.loads((legacy_dir / 'summary.json').read_text())
    command = [str(ROOT / 'build/run_btc15m_lead_lag_replay'), '--config', str(legacy_dir / 'config.json'),
               '--events', str(data / 'normalized' / slug / 'events.jsonl'),
               '--model', str(SUITE / 'frozen_model.json'), '--settlement', old_summary['winner'].lower(),
               '--trace', str(legacy_trace)]
    process = subprocess.run(command, text=True, capture_output=True, check=True, timeout=300)
    rerun = json.loads(process.stdout)
    if any(old_summary[k] != v for k, v in rerun.items()):
        raise ValueError('frozen replay summary mismatch')
    write(dest / 'legacy_rerun.json', {'summary_exact_match': True, 'command': command, 'summary': rerun})
    audit_path = SUITE / 'fill_conditional' / window / slug / 'execution.jsonl.gz'
    with gzip.open(audit_path, 'rt') as f:
        records = [json.loads(s) for s in f]
    orders = {r['id']: r for r in records if r['type'] == 'order'}
    fills = [r for r in records if r['type'] == 'fill']
    proposals = {}
    with legacy_trace.open() as f:
        for line in f:
            r = json.loads(line)
            for q in r.get('proposed_quotes', []):
                proposals[q['quote_id']] = q
    # Reconstruct the exact displayed-quantity initialization used by the legacy
    # engine, not an improved raw-event decision-time approximation.
    decision_books = []
    last_source = -1
    with (data / 'normalized' / slug / 'events.jsonl').open() as f:
        for line in f:
            r = json.loads(line)
            if r['type'] == 'books' and r['event_ms'] >= last_source:
                last_source = r['event_ms']
                decision_books.append(r)
    book_times = [r['recv_ms'] for r in decision_books]
    received = timelines(rows, tokens, 'received')
    source = timelines(rows, tokens, 'source')
    reversed_received = timelines(rows, tokens, 'received', True)
    requests = []
    for ident, o in orders.items():
        p = proposals[ident]['price_tick']
        ti, side = o['channel'] // 2, 'BUY' if o['channel'] % 2 == 0 else 'SELL'
        requests.append((o['active_ms'], ti, side, p, ident))
    arrivals = arrival_probes(rows, tokens, requests)
    arrivals_before = arrival_probes(rows, tokens, requests, inclusive=False)
    arrival_ties_changed = sum(arrivals[k] != arrivals_before[k] for k in arrivals)
    fill_labels = []
    quantities = [0, 0, 0]
    sums = [0., 0., 0.]
    diff = [0., 0., 0.]
    legacy_q = [0, 0, 0]
    missing = [Counter(), Counter(), Counter()]
    clock_diff, tie_diff = 0, 0
    for f in fills:
        ti, side = f['channel'] // 2, 'BUY' if f['channel'] % 2 == 0 else 'SELL'
        kwargs = dict(at=f['event_ms'], p=f['price_tick'], side=side,
                      quantity=f['quantity_lots'], expiry=market['expiry_ms'],
                      max_age=config['label']['max_quote_age_ms'])
        primary = label(received[ti], **kwargs)
        exchange = label(source[ti], **kwargs)
        reversed_label = label(reversed_received[ti], **kwargs)
        for h in range(3):
            a = primary['horizons'][str(HORIZONS[h])]
            b = exchange['horizons'][str(HORIZONS[h])]
            c = reversed_label['horizons'][str(HORIZONS[h])]
            if a['E_usd_per_share'] != b['E_usd_per_share']:
                clock_diff += 1
            if a['E_usd_per_share'] != c['E_usd_per_share']:
                tie_diff += 1
            if a['valid']:
                q = f['quantity_lots']
                quantities[h] += q
                sums[h] += q * a['E_usd_per_share']
                if f['markout'][h] is not None:
                    diff[h] += q * (a['E_usd_per_share'] - f['markout'][h])
                    legacy_q[h] += q
            else:
                missing[h][a['invalid_reason']] += f['quantity_lots']
        fill_labels.append({'legacy_fill': f, 'received_label': primary,
                            'source_clock_sensitivity': exchange,
                            'reverse_tie_sensitivity': reversed_label,
                            'arrival_diagnostic': arrivals[f['order_id']]})
    by_order = {}
    queue_changed = 0
    for ident, o in orders.items():
        i = bisect.bisect_right(book_times, o['created_ms']) - 1
        snapshot = decision_books[i]['up' if o['channel'] // 2 == 0 else 'down'] if i >= 0 else None
        p = proposals[ident]['price_tick']
        level_side = 'bids' if o['channel'] % 2 == 0 else 'asks'
        old_queue = sum(q for px, q in snapshot[level_side] if px == p) if snapshot else None
        new_queue = arrivals[ident]['queue_ahead']
        changed = old_queue is not None and new_queue is not None and abs(old_queue - float(new_queue)) > 1e-6
        queue_changed += changed
        selected = [r['received_label'] for r in fill_labels if r['legacy_fill']['order_id'] == ident]
        by_order[str(ident)] = {
            'order': o, 'proposal': proposals[ident], 'arrival_diagnostic': arrivals[ident],
            'arrival_before_same_ms': arrivals_before[ident],
            'legacy_queue_at_submit': old_queue, 'arrival_queue_different': changed,
            'D_usd': {str(h): order_label(selected, h, complete_observation=False) for h in HORIZONS},
            'observation_complete': False,
            'quality': 'legacy fills; raw lifecycle not replayed; absent fill unknown, never 0'}
    write(dest / 'fills.json', fill_labels)
    write(dest / 'orders.json', by_order)
    # Full raw source/received neighborhoods make the selected fills explainable.
    selected_times = [f['event_ms'] for f in fills]
    with gzip.open(dest / 'event_trace.jsonl.gz', 'wt') as f:
        for r in rows:
            if any(t - 200 <= r['src'] <= t + 5100 or t - 200 <= r['recv'] <= t + 5100 for t in selected_times):
                f.write(json.dumps({k: (v.hex() if isinstance(v, bytes) else str(v)) if isinstance(v, (Decimal, bytes)) else v
                                    for k, v in r.items() if k not in ('timestamp', 'timestamp_received')}) + '\n')
    lag = sorted(r['recv'] - r['src'] for r in active)
    def quantile(frac):
        return lag[min(len(lag) - 1, int(frac * len(lag)))] if lag else None
    ticks = [r for r in rows if r['event_type'] == 'tick_size_change']
    source_times = [set(t.times) for t in source]
    tied_trades = sum(r['src'] in source_times[tokens[r['asset_id']]]
                      for r in active if r['event_type'] == 'last_trade_price')
    fractional = sum(Decimal(str(r['size'])) % 1 for r in active
                     if r['event_type'] == 'last_trade_price' and r['size'] is not None)
    transactions = {}
    for r in active:
        if r['event_type'] == 'last_trade_price' and r['transaction_hash']:
            transactions.setdefault(r['transaction_hash'], set()).add(r['asset_id'])
    gamma_path = data / 'gamma' / (str(market['start_ms'] // 1000) + '.json')
    gamma = json.loads(gamma_path.read_text())
    gm = next(m for m in gamma['markets'] if m['conditionId'] == market['condition_id'])
    fee_metadata = {k: gm.get(k) for k in ('orderPriceMinTickSize', 'orderMinSize', 'feesEnabled',
                    'makerBaseFee', 'takerBaseFee', 'makerRebatesFeeShareBps', 'feeType', 'feeSchedule')}
    summary = {
        **pilot, 'orders': len(orders), 'legacy_fills': len(fills),
        'legacy_fill_quantity': sum(f['quantity_lots'] for f in fills),
        'raw_rows': len(rows), 'active_rows': len(active), 'active_event_counts': dict(Counter(r['event_type'] for r in active)),
        'negative_receive_lag_rows': sum(r['recv'] < r['src'] for r in rows),
        'active_receive_lag_ms': {'p50': quantile(.5), 'p95': quantile(.95), 'p99': quantile(.99), 'max': max(lag, default=None)},
        'trade_same_ms_token_book_events': tied_trades,
        'transactions_reporting_both_tokens': sum(len(t) > 1 for t in transactions.values()),
        'historical_tick_changes': [{k: str(r[k]) for k in ('src', 'asset_id', 'old_tick_size', 'new_tick_size')} for r in ticks],
        'fractional_public_volume_lost_by_legacy_floor': str(fractional),
        'arrival_status_counts': dict(Counter(a['status'] for a in arrivals.values())),
        'legacy_fills_on_orders_crossing_at_arrival': sum(arrivals[f['order_id']]['status'] == 'post_only_rejected' for f in fills),
        'arrival_same_ms_sensitivity_orders': arrival_ties_changed,
        'arrival_vs_submission_queue_changed_orders': queue_changed,
        'label_clock_sensitivity_horizon_records': clock_diff,
        'label_reverse_tie_sensitivity_horizon_records': tie_diff,
        'valid_label_quantity': quantities, 'invalid_quantity_reasons': [dict(c) for c in missing],
        'weighted_E_cents_per_share': [100 * s / q if q else None for s, q in zip(sums, quantities)],
        'strict_minus_legacy_E_cents_matched_quantity': [100 * d / q if q else None for d, q in zip(diff, legacy_q)],
        'provenance': {'raw_sha256': {str(p.relative_to(ROOT)): sha(p) for p in paths},
                       'gamma_sha256': sha(gamma_path),
                       'legacy_summary_sha256': sha(legacy_dir / 'summary.json'),
                       'legacy_config_sha256': sha(legacy_dir / 'config.json'),
                       'legacy_trace_sha256': sha(legacy_trace),
                       'legacy_execution_sha256': sha(audit_path), 'raw_schemas': sorted(schemas)},
        'cached_gamma_fee_metadata_not_historical_proof': fee_metadata,
        'legacy_rerun_summary_exact_match': True,
        'new_strategy_PnL': None}
    write(dest / 'summary.json', summary)
    return summary


def run():
    config = freeze()
    if sha(ROOT / 'build/run_btc15m_lead_lag_replay') != config['original_executable_sha256']:
        raise ValueError('legacy executable changed')
    results = []
    for pilot in config['pilots']:
        result = audit_case(pilot, config)
        results.append(result)
        print(json.dumps({k: result[k] for k in ('slug', 'orders', 'legacy_fills', 'arrival_status_counts', 'valid_label_quantity')}), flush=True)
    gate = {'version': config['version'], 'passed': False, 'bulk_shadow_allowed': False,
            'value_model_training_allowed': False, 'strategy_upgrade_allowed': False,
            'blocking_items': [
                'Legacy engine still executes against 50ms reconstructed books, without source-book arrival validation.',
                'Raw execution lifecycle and complementary matching attribution are not yet integrated or validated.',
                'Archive has no order IDs, connection/sequence completeness watermark; no-fill requires qualified coverage policy.',
                'Initial historical tick and unified executable exit/fee/inventory policy not yet verified and frozen.'],
            'scope': 'Raw-event audit of legacy fills and arrival diagnostics, plus tested strict-label/queue primitives; no corrected PnL.',
            'pilot_results': results,
            'frozen_executable_unchanged': sha(ROOT / 'build/run_btc15m_lead_lag_replay') == config['original_executable_sha256']}
    write(OUT / 'gate.json', gate)
    print('Audit gate FAIL: no bulk dataset or model training authorized by this configuration.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['freeze', 'run'])
    args = parser.parse_args()
    freeze() if args.command == 'freeze' else run()

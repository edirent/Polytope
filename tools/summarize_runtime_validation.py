#!/usr/bin/env python3
"""Offline V0.2.0 evidence assembly; never loads an authentication environment."""
import collections
import argparse
import hashlib
import json
import math
import shutil
from pathlib import Path
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'runs/runtime_v020'
OUT = BASE / 'evidence_20261106'
RUNS = BASE / 'validation_release_20261106'

def sha(p):
    h = hashlib.sha256()
    with p.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

def load(p):
    return json.loads(p.read_text())

def rows(p):
    with p.open() as f:
        for line in f:
            yield json.loads(line)

def save(p, data):
    p.write_text(json.dumps(data, indent=2) + '\n')

def percentile(values, q):
    return sorted(values)[max(0, math.ceil(len(values) * q) - 1)] if values else None

def coverage(mode, first, last):
    transitions = list(rows(OUT / (mode + '_reconstruction_diagnostics.jsonl.admission.jsonl')))
    by_venue = collections.defaultdict(list)
    for r in transitions:
        by_venue[r['venue']].append(r)
    result = {}
    for venue, events in by_venue.items():
        admitted = 0
        outages = []
        unavailable_reasons = collections.Counter()
        for before, after in zip(events, events[1:] + [{'availability_ns': last}]):
            duration = max(0, after['availability_ns'] - before['availability_ns'])
            if before['admitted']:
                admitted += duration
            else:
                outages.append(duration / 1e9)
                unavailable_reasons[before['reason']] += duration / 1e9
        result[venue] = dict(admitted_seconds=admitted / 1e9,
                             unavailable_seconds=(last - first - admitted) / 1e9,
                             percent_admitted=100 * admitted / (last - first),
                             recovery_admissions=sum(r['admitted'] for r in events),
                             unavailable_seconds_by_entry_reason=dict(unavailable_reasons),
                             maximum_unavailable_interval_seconds=max(outages, default=0),
                             transition_count=len(events))
    current = {v: False for v in by_venue}
    cursor = first
    joint = 0
    for event in sorted(transitions, key=lambda x: (x['availability_ns'], x['input_index'])):
        if all(current.values()):
            joint += max(0, event['availability_ns'] - cursor)
        current[event['venue']] = event['admitted']
        cursor = event['availability_ns']
    if all(current.values()):
        joint += max(0, last - cursor)
    return dict(scope='First through last input availability; existing As-Of admission, not proof of exchange stream completeness or an economically approved pair.',
                window_seconds=(last - first) / 1e9, per_venue=result,
                both_markets_admitted_seconds=joint / 1e9,
                both_markets_admitted_percent=100 * joint / (last - first))

def experiment(mode):
    directory = RUNS / mode
    result = load(directory / 'operational_audit.json')
    result['latency_scope_notes']['ws_receive_to_decode'] = 'Legacy name pools all venue_packet callbacks, including REST and control events; not a WS-only series. WS market-only callback-to-sequencer values are callback_to_commit_quantile_lower_bounds.'
    dashboard = load(directory / 'dashboard.json')
    raw = [x['event'] for x in rows(directory / 'inputs.jsonl')]
    diagnostics = list(rows(OUT / (mode + '_reconstruction_diagnostics.jsonl')))
    counts = collections.Counter((r.get('reason'), r.get('venue'), r.get('transport'), r.get('apply_rejection')) for r in diagnostics)
    counts_list = [dict(reason=k[0], venue=k[1], transport=k[2], apply_rejection=k[3], count=v)
                   for k, v in sorted(counts.items(), key=lambda x: str(x[0]))]
    save(OUT / (mode + '_diagnostic_counts.json'), counts_list)
    result['reconstruction_diagnostics'] = counts_list
    result['primary_crossed_or_locked_frames'] = len({r['input_index'] for r in diagnostics if r.get('apply_rejection') == 'CROSSED_OR_LOCKED_BOOK'})
    result['coverage'] = coverage(mode, raw[0]['recv_ns'], raw[-1]['recv_ns'])
    histories = list(rows(directory / 'health_history.jsonl'))
    result['maximum_sampled_queue_depth'] = max(r['source_health']['queue_depth'] for r in histories)
    result['queue_overflow_observed'] = any(r['source_health']['overflow'] for r in histories)
    result['dropped_at_shutdown'] = dashboard['runtime']['source_health']['dropped']
    result['terminal_health'] = dashboard['runtime']['health']
    first_event = next(rows(directory / 'events.jsonl'))['event']
    start = first_event['started_wall_ns'] / 1e9
    result['start_local'] = datetime.fromtimestamp(start, timezone.utc).astimezone(ZoneInfo('America/Chicago')).isoformat()
    result['end_local_approximate'] = datetime.fromtimestamp(start + result['actual_duration_seconds'], timezone.utc).astimezone(ZoneInfo('America/Chicago')).isoformat()
    streams = {}
    send_delays = []
    delta_delays = []
    reconnects = collections.Counter()
    rejected_bbo_examples = []
    bad = {r['input_index'] for r in diagnostics if r.get('apply_rejection') == 'CROSSED_OR_LOCKED_BOOK'}
    for index, row in enumerate(raw):
        if row.get('transport') == 'event':
            reconnects[(row['venue'], row['classification'])] += 1
        if row.get('transport') != 'ws' or row.get('classification') != 'market_data':
            continue
        try:
            packet = json.loads(row['raw_payload'])
        except ValueError:
            continue
        if index in bad and len(rejected_bbo_examples) < 3:
            rejected_bbo_examples.append(dict(input_index=index, raw_hash=hashlib.sha256(row['raw_payload'].encode()).hexdigest(), wire=packet))
        if row['venue'] != 'kalshi' or not isinstance(packet, dict) or packet.get('type') not in ['orderbook_snapshot', 'orderbook_delta']:
            continue
        key = (row['connection_epoch'], packet['sid'])
        msg = packet['msg']
        s = streams.setdefault(key, dict(epoch=key[0], sid=key[1], first_type=packet['type'], first_seq=packet['seq'],
                                        previous_seq=None, previous_clock=None, last_seq=None,
                                        snapshots=0, deltas=0, nonconsecutive_sequences=0, clock_regressions=0))
        seq, clock = packet['seq'], msg.get('clock')
        if s['previous_seq'] is not None and seq != s['previous_seq'] + 1:
            s['nonconsecutive_sequences'] += 1
        if clock is not None and s['previous_clock'] is not None and clock < s['previous_clock']:
            s['clock_regressions'] += 1
        s['previous_seq'] = s['last_seq'] = seq
        if clock is not None:
            s['previous_clock'] = clock
        s['snapshots' if packet['type'] == 'orderbook_snapshot' else 'deltas'] += 1
        if packet.get('sending_ts_ms'):
            send_delays.append(row['recv_wall_ns'] - packet['sending_ts_ms'] * 1000000)
        if msg.get('ts_ms'):
            delta_delays.append(row['recv_wall_ns'] - msg['ts_ms'] * 1000000)
    result['kalshi_book_streams'] = list(streams.values())
    result['kalshi_wall_clock_delay'] = dict(scope='Local callback wall clock minus server timestamp; not clock-synchronized one-way latency.',
                                            sending_timestamp_samples=len(send_delays),
                                            sending_delay_p50_ns=percentile(send_delays, .5),
                                            sending_delay_p99_ns=percentile(send_delays, .99),
                                            event_timestamp_samples=len(delta_delays),
                                            event_delay_p50_ns=percentile(delta_delays, .5),
                                            event_delay_p99_ns=percentile(delta_delays, .99))
    result['reconnect_events'] = [dict(venue=k[0], classification=k[1], count=v) for k, v in reconnects.items()]
    result['reconstruction_root_cause_status'] = 'UNRESOLVED: local depth contradicted wire best_bid/best_ask at some primary rejection frames. Cascading deltas after invalidation are not measured packet loss.'
    result['acceptance_scope'] = 'Receipt + deterministic replay gate passed; continuous correct L2 completeness and production readiness are NOT certified.'
    save(OUT / (mode + '_rejected_bbo_examples.json'), rejected_bbo_examples)
    return result

def report(result):
    p, s = [result['experiments'][m] for m in ['paper', 'shadow']]
    text = ['# Polytope V0.2.0 — observed release evidence',
            'Date: 2026-11-06, America/Chicago. These are completed bounded experiments; no collection remains running. Runtime receipt/replay gates passed. Continuous complete L2 correctness and production readiness remain unverified.',
            '## Implemented path',
            'Extended existing native runtime, Decode/sequence-aware state, SignalEngine, OrderDecisionEngine, RiskEngine, ExecutionGateway and PaperLedger. No second OMS, strategy engine, portfolio authority or accounting framework. REPLAY/PAPER/SHADOW reuse interfaces; LIVE is rejected. Raw input and financial journals are SHA-chained. Full authorization/plan and submit markers are durable before the Paper adapter. Known receipts restore exactly once; unknown acknowledgement freezes exposure and never blindly retries.',
            'The live facade is the sole authoritative mutator for venue-qualified books. Timer-driven stale admission and bounded ingress are independent of exchange traffic. High-volume non-economic audit checkpoints target 250 ms; critical order/receipt events force raw dependency and financial fsync before proceeding. Read-only operations reuse paper_backend and the existing dashboard.',
            '## Real observations',
            '| Metric | PAPER | SHADOW |', '|---|---:|---:|']
    fields = [('Actual seconds', 'actual_duration_seconds'), ('Recorded/committed inputs', 'processed_inputs'),
              ('Input callbacks including timers/diagnostics', 'strategy_callbacks'), ('Risk-decision records', 'risk_decisions'),
              ('Approvals', 'risk_approvals'), ('Paper submissions', 'paper_submissions'),
              ('Simulated fill receipts', 'simulated_fill_receipts'), ('Maximum sampled queue depth', 'maximum_sampled_queue_depth'),
              ('Dropped at shutdown', 'dropped_at_shutdown')]
    for name, key in fields:
        text.append(f'| {name} | {p[key]} | {s[key]} |')
    for label, key in [('Kalshi snapshot', 'kalshi:orderbook_snapshot'), ('Kalshi delta', 'kalshi:orderbook_delta'),
                       ('Polymarket book objects', 'polymarket_international:book'), ('Polymarket price_change objects', 'polymarket_international:price_change')]:
        text.append(f"| {label} | {p['wire_message_counts'][key]} | {s['wire_message_counts'][key]} |")
    text += [f"Start PAPER: {p['start_local']}; SHADOW: {s['start_local']}. End approximately {p['end_local_approximate']} / {s['end_local_approximate']}.",
             'Kalshi ticker: `KXBTCD-26NOV0717-T76249.99`. Polymarket Gamma ID: `6667467`; condition: `0xc52d0ad0d001293473fa50126015c9d6db3baab756002f7b4ccf9eb179b1c403`; question: US x Iran effective ceasefire continues through November 8, 2026? Metadata and token IDs are frozen in the selection/configuration files.',
             'These are independent feed probes, not an approved cross-venue economic pair. Existing combinatorial strategy evaluated a Polymarket same-condition YES/NO full-set paper artifact. It found no positive opportunity and no approved orders. Public last_trade_price messages were never treated as own fills.',
             '## Integrity, gaps and usable coverage',
             '| Availability-event reconstructed admission | PAPER | SHADOW |', '|---|---:|---:|']
    for venue in ['kalshi', 'polymarket_international']:
        x, y = [a['coverage']['per_venue'][venue] for a in [p, s]]
        text.append(f"| {venue} | {x['admitted_seconds']:.6f} s ({x['percent_admitted']:.4f}%) | {y['admitted_seconds']:.6f} s ({y['percent_admitted']:.4f}%) |")
    text.append(f"| Both markets | {p['coverage']['both_markets_admitted_seconds']:.6f} s ({p['coverage']['both_markets_admitted_percent']:.4f}%) | {s['coverage']['both_markets_admitted_seconds']:.6f} s ({s['coverage']['both_markets_admitted_percent']:.4f}%) |")
    text += ['This spans first through last recorded sequencer availability and reruns the same existing state/As-Of rules at every input. It is admission under the configured rule, not proof that the exchange stream was complete. Simultaneous admission is not economic pair approval. Approximately 1 Hz dashboard health is a different sampling measure; exact durations and per-reason outages are in summary.json and admission JSONL.',
             'Kalshi book messages had zero observed sequence jumps or exchange-clock regressions within each epoch/SID, with snapshots first. PAPER had 2 epochs; SHADOW 3. Both recovered the configured 120-second reconnect. SHADOW also rebuilt after early invalid-time diagnostics. Authority expiry caused repeated conservative interruptions: authority lifetime and REST polling were both 15 seconds, leaving gaps for response delays/inconclusive comparisons. REST remained diagnostic and never substituted for a continuous WebSocket stream.',
             'Polymarket had 23 epochs in each session. PAPER had 22 validation_reconnect events; SHADOW had 20 plus 2 timeouts. Counts include a configured controlled reconnect. Each session had 20 primary crossed/locked frames (40 per-outcome rejection records). After invalidation, PAPER rejected 98 and SHADOW 90 delta records; these cascades do not establish packet loss. PAPER had 1 authoritative REST mismatch. SHADOW had 2 Kalshi REST and 2 Polymarket WS invalid-time/hash records. There were 30 inconclusive REST comparisons in each session. Precise reasons/indices/raw hashes and example rejected wire BBO are saved.',
             'On some primary failures, wire price_change advertised an uncrossed best bid/ask while local full-depth reconstruction crossed. Upstream completeness versus local reconstruction/protocol cause is unresolved. No level-pruning repair or fabricated message-gap count is used. Admission failed closed and fresh snapshots restored it. This is a blocker to claiming reliably continuous correct L2.',
             'Maximum inter-market-message silence is reported per venue in summary.json. Silence is not proof of loss; Polymarket public messages have no exchange sequence. Last shutdown disconnects are explicit controlled events.',
             '## Accounting, OMS and replay',
             'Both original input and financial SHA chains verified, with zero incomplete transactions. PAPER compared 46,190 and SHADOW 45,943 replay commits: every decision-trace and accounting digest matched. Replays use the same frozen executable, immutable inputs and original execution policy, with Kalshi environment variables removed. See each run/replay_comparison.json.',
             'Both accounts remain at funded 100,000,000 ticks, no positions, fees, reservations or simulated realized/marked PnL. `verified_live_realized_pnl=null`. Private exchange-account reconciliation is UNCONFIGURED. No cancellation/settlement or exchange receipt is inferred. The 199-plan benchmark below is synthetic paper execution only.',
             '## Latency measurements',
             'Values below are milliseconds, measured with local monotonic clocks; blank/NOT_OBSERVED is not zero. p99.9 requires at least 10,000 samples. `end_to_end` is consumer transaction processing and excludes ingress/raw persistence and dashboard. `book_update_to_strategy` starts at decoder entry and overlaps decode; stage percentiles are not additive. The legacy-named `ws_receive_to_decode` pools ALL venue_packet callbacks, including REST and control events; it is not an isolated WS-only series. Exact WS market callback-to-sequencer measurements are also the saved callback-to-commit lower bounds.',
             '| Stage | PAPER samples | PAPER p50 / p95 / p99 / p99.9 ms | SHADOW samples | SHADOW p50 / p95 / p99 / p99.9 ms |',
             '|---|---:|---|---:|---|']
    def numbers(stats):
        return ' / '.join('—' if stats.get(k) is None else f'{stats[k]/1e6:.3f}' for k in ['p50', 'p95', 'p99', 'p99_9'])
    stages = ['ws_receive_to_decode', 'decode_to_book_state', 'book_update_to_strategy', 'strategy_to_risk',
              'risk_to_durable_plan', 'durable_plan_to_adapter', 'journal_write', 'journal_fsync',
              'raw_persistence', 'raw_fsync', 'dashboard_publication', 'end_to_end']
    for stage in stages:
        x, y = p['latency'][stage], s['latency'][stage]
        text.append(f"| {stage} | {x['samples']} | {numbers(x)} | {y['samples']} | {numbers(y)} |")
    text += ['Real runs had no approved orders, so the three order-path stage latencies are NOT_OBSERVED. The offline 200-input synthetic profitable fixture exercised 199 approved Paper plans. It is not real-market profit or live trading. Synthetic p50/p95/p99/p99.9 ms:',
             '| Synthetic stage | Samples | Percentiles ms |', '|---|---:|---|']
    for stage in ['strategy_to_risk', 'risk_to_durable_plan', 'durable_plan_to_adapter', 'end_to_end']:
        x = result['synthetic_order_path']['latency'][stage]
        text.append(f"| {stage} | {x['samples']} | {numbers(x)} |")
    text += ['Callback-to-commit exact combined percentiles are unavailable. Single-writer ordering bounds each commit between its input availability and the next; quantile lower/upper bounds are saved in summary.json. Upper bounds include idle/telemetry, so they are not measured exact execution latencies. Server-to-local wall-clock differences are negative (approximately −110 ms median relative to sending_ts_ms); clock offset is not calibrated, so these are not one-way network latency.',
             'Initial fully synchronous development captures were stopped after roughly 253 s when queues grew; their evidence is retained as incomplete diagnostics. Batch checkpoints removed that observed backlog in this load/window; storage fsync and atomic dashboard writes remain blocking I/O. The 30-minute runs do not establish multi-day capacity or worst-case real-time guarantees.',
             '## Tests and release reproduction',
             'Full CTest: 1,039 registered / 1,034 executed / 1,033 passed / 1 known pre-existing failed / 5 disabled. Failure: AvalancheSkewsQuotesAndRiskCutsBuyChannel — reducing ask below fair. No unrelated strategy adjustment was made. Operational fault suite: all 42 scenarios passed (includes inherited 24). Native lifecycle: 21 assertions passed. TypeScript/Vite build and git diff --check passed.',
             'Fault artifacts cover duplicate market/intent/report delivery, invalid snapshots/status, disconnect and new snapshots, missing heartbeat/stale data, sequence gaps, queue-delay freshness, pre-submit/during-submit/after-receipt crash recovery, ENOSPC/partial writes, ledger mismatch, durable stop/kill latch, strategy exception, slow operations clients and unresolved restart.',
             f"Frozen source: {result['source_file_count']} files, SHA-256 `{result['source_tree_sha256']}`. Executable SHA-256 `{result['binary_sha256']}`. Both fresh independent rebuilds matched byte-for-byte. Source manifest entries and ordered tree hash verified. Final analysis scripts/reports are separately hashed outside the runtime freeze; native source has not drifted.",
             'Five scoped changeset patches apply successfully against a disposable captured-baseline copy, without staging/committing the user worktree. The patch manifest verifies unrelated pre-existing dirty files are unchanged. Credential/private-key/runtime secret files are excluded. Build/test logs and compiler/dependency identities are preserved.',
             '## Limits and next migration',
             'Operational PAPER/SHADOW recording/recovery is validated for this finite workload. Execution/fills/fault recovery are paper simulations. Continuous Polymarket reconstruction correctness, private account reconciliation, live orders/acknowledgements/cancels/settlement, maker integration, software upgrades/host reboot and multi-day journal retention remain unverified. Do not call this production-ready.',
             'Legacy schema-v1 aggregate feed_connected/reconnects/fresh_snapshot_assets/decode_errors/state_errors are not authoritative summaries of live facade state. Use source_health, venue_counters, markets[].admitted and runtime.health. The stopped snapshots correctly expose disconnected per-venue sources; legacy field consolidation remains a known display limitation.',
             'Retain MarketMakingEngine, QuoteIntent/CancelQuoteIntent, QuoteRiskEvaluator and maker adapters. Next, extend the existing OMS with asynchronous maker request/report lifecycle and global actual-size reservations; test cancel/replace/partial-fill races and preserve unknown encumbrance. Do not convert passive quotes into taker bundles or enable maker/live execution through configuration alone.',
             '## Inspect and reproduce',
             '```bash\npython3 tools/audit_operational_run.py --run runs/runtime_v020/validation_release_20261106/paper --binary runs/runtime_v020/release_v020_20261106/trading_runtime --with-replay\n# Repeat with shadow. New replay outputs preserve prior evidence; no credentials are needed.\npython3 tools/summarize_runtime_validation.py\npython3 tools/prepare_runtime_changesets.py\npython3 tools/summarize_runtime_validation.py --inventory-only\n```',
             'Serve the existing readonly UI against completed PAPER evidence (two terminals):',
             '```bash\nbuild/paper_backend --dashboard-file runs/runtime_v020/validation_release_20261106/paper/dashboard.json --host 127.0.0.1 --port 8080\ncd frontend/dashboard && npm run dev\n```',
             'Open http://localhost:5173/?view=runtime. A completed run intentionally displays STOPPED. New live capture requires explicit finite profiles and local authenticated terminal context; do not silently reuse old market validity or start an unrequested long soak.',
             'Detailed contracts: docs/operational_runtime_v020.md, docs/oms_durability_audit.md, docs/release_reproducibility.md, docs/runtime_soak_test_plan.md. Primary evidence: summary.json, audit_manifest.json, changesets/manifest.json, operational_faults.log, fault_evidence/, paper/shadow reconstruction/admission JSONL, and each real run\'s journals/audits/replay comparison.']
    (OUT / 'report.md').write_text('\n\n'.join(text).replace('|\n\n|', '|\n|') + '\n')

def inventory():
    analysis = ['tools/' + n for n in ['freeze_runtime_release.py', 'prepare_runtime_validation.py',
                'launch_runtime_validation.py', 'audit_operational_run.py', 'runtime_feed_diagnostics.cpp',
                'summarize_runtime_validation.py', 'prepare_runtime_changesets.py']]
    analysis += ['docs/' + n for n in ['operational_runtime_v020.md', 'oms_durability_audit.md',
                 'release_reproducibility.md', 'runtime_soak_test_plan.md']]
    overlay = []
    for rel in analysis:
        target = OUT / 'analysis_overlay' / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / rel, target)
        overlay.append(dict(path=rel, sha256=sha(target)))
    save(OUT / 'analysis_overlay_manifest.json', dict(scope='Final analysis/report tools; separate from frozen runtime executable source', files=overlay))
    files = [p for directory in [OUT, RUNS] for p in directory.rglob('*')
             if p.is_file() and p.name != 'audit_manifest.json' and '__pycache__' not in p.parts]
    files += [BASE / 'release_ready.json', BASE / 'local_launch_results.json',
              BASE / 'release_v020_20261106/release_manifest.json', BASE / 'release_v020_20261106/trading_runtime',
              BASE / 'prechange_20261106/dirty_inventory.json', BASE / 'prechange_20261106/sensitive_scan.json',
              BASE / 'baseline_20261106/release_manifest.json']
    entries = [dict(path=str(p.relative_to(ROOT)), bytes=p.stat().st_size, sha256=sha(p)) for p in sorted(set(files))]
    digest = hashlib.sha256(''.join(r['path'] + '\0' + r['sha256'] + '\n' for r in entries).encode()).hexdigest()
    save(OUT / 'audit_manifest.json', dict(schema_version=1, files=entries, inventory_sha256=digest,
         self_excluded=True, frozen_source='runs/runtime_v020/release_v020_20261106/release_manifest.json',
         notes='Original and replay data, diagnostic development runs, final analysis overlay and test logs. Source entries verified separately; no credentials included.'))
    print(json.dumps(dict(evidence_files=len(entries), inventory_sha256=digest)))

def main():
    release = BASE / 'release_v020_20261106'
    manifest = load(release / 'release_manifest.json')
    entries = manifest['files']
    assert all(sha(release / 'source' / e['path']) == e['sha256'] for e in entries)
    tree = hashlib.sha256(''.join(e['path'] + '\0' + e['sha256'] + '\n' for e in entries).encode()).hexdigest()
    assert tree == manifest['source_tree_sha256']
    binaries = {name: sha(path) for name, path in {
        'release': release / 'trading_runtime',
        'independent_a': BASE / 'rebuild_final_a/trading_runtime',
        'independent_b': BASE / 'rebuild_final_b/trading_runtime'}.items()}
    assert len(set(binaries.values())) == 1 and binaries['release'] == manifest['binary_sha256']
    # Analysis and final reports are intentionally outside the runtime freeze.
    drift = [e['path'] for e in entries if not (ROOT / e['path']).exists() or sha(ROOT / e['path']) != e['sha256']]
    assert not any(p.startswith('engine/') or p == 'CMakeLists.txt' for p in drift), 'frozen native source drift'
    synthetic = load(OUT / 'synthetic_latency/run/dashboard.json')
    result = dict(release='0.2.0', date_local='2026-11-06', source_tree_sha256=tree, source_file_count=len(entries),
                  binary_sha256=binaries['release'], independent_rebuild_hashes=binaries,
                  frozen_source_verified=True, post_freeze_existing_file_changes=drift,
                  tests=dict(registered=1039, executed=1034, passed=1033, failed=1, disabled=5,
                             known_preexisting_failure='AvalancheSkewsQuotesAndRiskCutsBuyChannel: reducing ask below fair',
                             operational_scenarios=42, operational_passed=42, native_lifecycle_assertions=21, frontend_build='PASS'),
                  experiments={m: experiment(m) for m in ['paper', 'shadow']},
                  synthetic_order_path=dict(label='SYNTHETIC PROFITABLE FIXTURE; NOT REAL MARKET PROFIT',
                                            paper_submissions=synthetic['runtime']['paper_submissions'],
                                            approvals=synthetic['runtime']['approvals'],
                                            latency=load(OUT / 'synthetic_latency/run/latency.json')),
                  real_order_submission='DISABLED', verified_live_realized_pnl=None,
                  private_reconciliation='UNCONFIGURED', production_ready=False)
    save(OUT / 'summary.json', result)
    report(result)
    print(json.dumps(dict(binary_sha256=binaries['release'], rebuilds_match=True,
                         experiment_seconds={k: v['actual_duration_seconds'] for k, v in result['experiments'].items()},
                         joint_admission_seconds={k: v['coverage']['both_markets_admitted_seconds'] for k, v in result['experiments'].items()},
                         native_source_drift=False)))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory-only', action='store_true', help='Freeze analysis overlay and hash evidence after patches/logs are final')
    args = parser.parse_args()
    inventory() if args.inventory_only else main()

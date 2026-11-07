#!/usr/bin/env python3
"""Freeze review changes, tested identities and actual V021 A-J report."""
import collections,csv,difflib,hashlib,importlib.util,json,shutil,subprocess,tempfile
from pathlib import Path
from datetime import datetime,timezone
from zoneinfo import ZoneInfo
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'runs/runtime_v0_2_1_20261106'
def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()
def load(p):return json.loads(p.read_text())
def save(p,v):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(v,indent=2)+'\n')
def changes():
    baseline=OUT/'baseline/source';manifest=load(OUT/'baseline/release_manifest.json');prior={e['path']:e for e in manifest['files']}
    spec=importlib.util.spec_from_file_location('freeze',ROOT/'tools/freeze_runtime_release.py');freeze=importlib.util.module_from_spec(spec);spec.loader.exec_module(freeze)
    result=[];patches=collections.defaultdict(list)
    for p in freeze.collect():
        rel=str(p.relative_to(ROOT));before=baseline/rel
        if before.exists() and sha(before)==sha(p):continue
        if rel.startswith(('engine/runtime/','engine/common/','config/')) or rel=='CMakeLists.txt':group='01_runtime'
        elif rel.startswith(('engine/risk/','engine/execution/')):group='02_risk_oms'
        elif rel.startswith('engine/paper/'):group='03_accounting'
        elif rel.startswith(('apps/','frontend/')):group='04_operations'
        else:group='05_tests_docs_tools'
        old=before.read_text().splitlines(keepends=True) if before.exists() else [];new=p.read_text().splitlines(keepends=True)
        header=f'diff --git a/{rel} b/{rel}\n'+('new file mode 100644\n' if not before.exists() else '')
        patch=header+''.join(difflib.unified_diff(old,new,fromfile='a/'+rel if before.exists() else '/dev/null',tofile='b/'+rel))
        patches[group].append(patch);result.append(dict(path=rel,before_sha256=sha(before) if before.exists() else None,after_sha256=sha(p),group=group))
    changed={r['path'] for r in result}
    for rel,e in prior.items():
        if rel not in changed:assert (ROOT/rel).exists() and sha(ROOT/rel)==e['sha256'],rel
    (OUT/'changesets').mkdir(exist_ok=True);checks=[]
    with tempfile.TemporaryDirectory(prefix='polytope-v021-review-') as temp:
        target=Path(temp)/'source';shutil.copytree(baseline,target)
        for group,parts in sorted(patches.items()):
            path=OUT/'changesets'/(group+'.patch');path.write_text(''.join(parts));subprocess.run(['git','apply','--check',str(path)],cwd=target,check=True);subprocess.run(['git','apply',str(path)],cwd=target,check=True)
            checks.append(dict(path=str(path.relative_to(OUT)),sha256=sha(path),apply_check='PASS'))
        assert all(sha(target/r['path'])==r['after_sha256'] for r in result)
    # The old, user-modified maker economics and research runners were preserved.
    preserved=[e for rel,e in prior.items() if rel.startswith(('engine/strategy/market_making/','research/','replay/market_making/'))]
    assert all(sha(ROOT/e['path'])==e['sha256'] for e in preserved)
    save(OUT/'implementation_changes.json',dict(changes=result,patches=checks,unmodified_baseline_source_files=len(prior)-len([p for p in changed if p in prior]),
        maker_algorithms_and_existing_research_unchanged=True,git_staging=False,commits_created=False))
def live_health():
    result={}
    for folder in (OUT/'experiments_rc3').glob('live*/*'):
        if not (folder/'recording_manifest.json').exists():continue
        key=folder.parent.name+'/'+folder.name;manifest=load(folder/'recording_manifest.json');hist=[json.loads(s) for s in (folder/'health_history.jsonl').read_text().splitlines()]
        duration=collections.Counter()
        for a,b in zip(hist,hist[1:]):duration[a['health']]+=max(0,b['logical_now_ns']-a['logical_now_ns'])/1e9
        counts=collections.Counter();epochs=set();messages=0;max_queue=0
        for h in hist:max_queue=max(max_queue,h['source_health'].get('queue_depth',0))
        for s in (folder/'inputs.jsonl').open():
            row=json.loads(s)['event']
            if row.get('transport')!='ws' or row.get('classification')!='market_data':continue
            messages+=1;epochs.add(row['connection_epoch'])
            try:v=json.loads(row['raw_payload'])
            except ValueError:continue
            for x in v if isinstance(v,list) else [v]:
                if isinstance(x,dict):counts[x.get('event_type',x.get('type','unknown'))]+=1
        with (folder/'events.jsonl').open() as f:first=json.loads(f.readline())['event']
        start=datetime.fromtimestamp(first['started_wall_ns']/1e9,timezone.utc).astimezone(ZoneInfo('America/Chicago')).isoformat()
        result[key]=dict(actual_seconds=manifest['actual_elapsed_ns']/1e9,start_local=start,ws_market_frames=messages,wire_objects=dict(counts),epochs=sorted(epochs),
            sampled_health_seconds=dict(duration),maximum_sampled_queue=max_queue,note='Approximately 1 Hz health; not exact continuous L2 coverage or proof of packet-loss absence')
    save(OUT/'live_health_audit.json',result);return result
def latency_row(stats):
    return ' / '.join('—' if stats.get(k) is None else f'{stats[k]/1e6:.3f}' for k in ['p50','p95','p99','p99_9'])
def report(health):
    runs=load(OUT/'portfolio_pnl_summary.json')['runs'];tests=load(OUT/'test_summary.json');det=load(OUT/'determinism_audit.json');performance=load(OUT/'performance_comparison.json')
    with (OUT/'strategy_parity.csv').open() as f:parity=list(csv.DictReader(f))
    mismatches=sum(any(r[k]!='1' for k in ['result_hash_equal','quotes_equal','cancels_equal']) for r in parity)
    build=load(OUT/'release_rc3/release_manifest.json');binary=OUT/'release_rc3/trading_runtime'
    assert sha(binary)==build['binary_sha256']==sha(OUT/'rebuild_rc3/trading_runtime')
    for e in build['files']:assert sha(OUT/'release_rc3/source'/e['path'])==e['sha256']
    native_drift=[e['path'] for e in build['files'] if e['path'].startswith('engine/') and sha(ROOT/e['path'])!=e['sha256']];assert not native_drift
    assert sha(ROOT/'runs/runtime_v020/validation_release_20261106/paper/inputs.jsonl')==load(OUT/'recording_lineage.json')['original_sha256']
    save(OUT/'build_manifest.json',dict(runtime_release='release_rc3/',source_tree_sha256=build['source_tree_sha256'],binary_sha256=sha(binary),independent_rebuild_sha256=sha(OUT/'rebuild_rc3/trading_runtime'),
        byte_identical_rebuild=True,all_frozen_source_entries_verified=True,native_source_drift=native_drift,compiler=build['compiler'],dependencies=build['dependencies'],build_profile=build['build_profile'],
        configuration_hashes={p.name:sha(p) for p in (OUT/'profiles').glob('*.json')},recording_lineage=load(OUT/'recording_lineage.json'),
        analysis_overlay='analysis_overlay/',notes='Final report/doc appendices and analysis tools are separately hashed. Native runtime source matches the tested release. Earlier candidates and financial journals remain archived.'))
    text=['# Polytope V0.2.1 — actual implementation and validation','Readiness decision: **PARTIALLY_VERIFIED**. Date: 2026-11-06, America/Chicago. All collection is bounded and stopped. No exchange orders or private account receipts exist.',
      '## A. Architecture',
      'Before: maker research runner → MarketMakingEngine/QuoteRiskEvaluator → runner-local PaperMakerExecutionAdapter/ActivePaperQuoteBook → runner-local ledger. After: existing Session → authoritative state facade → StrategyPlugin/StrategySupervisor → typed StrategyRequest → existing RiskEngine/ReservationBook → existing ExecutionGateway → PaperAtomic / managed PaperMakerExecutionAdapter → canonical PaperTradingEngine/PaperLedger → existing backend/dashboard.',
      'Maker algorithms, fair/inventory/reward/product code and old research runners are unchanged from the captured dirty baseline. Managed adapter calls evaluate immutable OMS-owned projections and do not populate the legacy quote store. Old runner state remains isolated research truth, not the unified account. The gateway is actual order authority; the plugin owns desired state only.',
      '## B. Multi-strategy behavior',
      'Combinatorial and generic maker plugins run in one native single-writer process, sharing books, risk, OMS and ledger. Each has instance/epoch/model identity, explicit budget/subscriptions and deterministic priority. Native executable faults prove competing cash/inventory admission, strategy-local stop and exception containment. Broader specialized BTC provider orchestration is not enabled or certified.',
      'The historical source is an immutable 11,609-row hash-chain prefix spanning 599.796420548 seconds, not newly collected live data. It contains both prior Kalshi/Polymarket feeds; only Polymarket is funded for this milestone. The original V0.2.0 source hash was verified unchanged. No cross-venue pair was automatically approved.',
      '## C. OMS correctness',
      'Durable risk reservation → full prepared identity/parameters → submit marker → adapter marker precedes simulated side effects. Multiple independent children support positive submission latency, ACK_UNKNOWN, partial fills, cancel-pending resources, cancellation rejection/timeout, conservative or overlap replacement and confirmed simulated expiry. Stable receipt IDs deduplicate later delivery without tuple identity. Known tail fills restore once; possible submission remains RECOVERY_BLOCKED and is never blindly resent.',
      f"All {len(det['runs'])} completed native recoveries matched every decision/account commit digest and appended no duplicate fills. Fault evidence retains expected crash/refusal exit codes and unresolved tails. No local event is called an exchange cancellation or settlement.",
      '## D. Portfolio risk',
      'Actual sized bundle quantities are rebound into the existing reservation. Managed commitments reserve buy cash/fees or sell inventory. Filled inventory and unfilled resources are distinct. Global, exact-condition event, venue/account, strategy and asset/order limits are checked atomically. Cancel/unknown resources never expire on local timers. Unexplained exposure or mismatch freezes increases. No title-based netting or cross-wallet collateral offset exists.',
      '## E. Accounting',
      'Independent audit reconstructed all simulated cash movements, fees, positions/cost and outstanding commitments and matched canonical native outputs for every completed run. RC1 exposed an unsupported sell in the valuation projection; execution stopped and evidence remains. RC2 reuses existing PositionLedger::apply_sell and verifies the derived projection against canonical inventory, with a new complete buy/sell/restart test. The corrected release passed final regressions.',
      '`verified_live_realized_pnl=null`, private account reconciliation UNCONFIGURED. Rewards are not invented, terminal payoff is not cash, and settled_cash_pnl remains null. Existing ledger realized-PnL conventions and separately posted buy fees are distinguished from net equity. Unusable marks are not reported as verified economic profit.',
      '## F. Maker parity',
      f"{len(parity):,} native immutable frames compared, {mismatches} differences in result hashes, full desired quotes or cancel hashes. This compares the unchanged generic legacy maker runner core under identical frozen configuration, clock, inventory, depth and OMS feedback. It does **not** certify all old BTC 5m/15m product-provider/external-fair runner orchestration. Native desired quotes remain separate from explicit outward tick rounding and shared-risk execution rejections.",
      '## G. Execution simulation',
      'Four independently funded scenarios use existing fill-model code: optimistic book touch, conservative aggressor trade-through, displayed-queue approximation, and adverse movement with delayed cancellation. Source evidence cannot fill before configured arrival; REST/heartbeat/unrelated updates are not fill events. Queue assumptions are not exact FIFO. Values are ticks (1,000,000 ticks = 1 USDC); all fills/PnL below are **simulated**.',
      '| Data | Model | Fill receipts | Filled lots | Fees ticks | Net equity PnL ticks | Peak reserved capital |', '|---|---|---:|---:|---:|---:|---:|']
    for r in runs:
        if r['scope'] not in ['synthetic','real_recorded']:continue
        text.append(f"| {r['scope']} | {r['model']} | {r['maker_fill_receipts']} | {r['maker_filled_lots']} | {r['fees_tick']} | {r['net_equity_pnl_tick']} | {100*r['peak_reserved_capital_utilization']:.3f}% |")
    text += ['Synthetic profit is a constructed fixture result, not strategy profitability. Historical real optimistic/conservative/queue scenarios had no fill; adverse stress lost 150,000 ticks (0.15 USDC). Fill rates, markouts, drawdown, inventory, lifetime and capacity at observed sizes are in model_comparison and markout CSVs; unavailable horizons stay unavailable.',
      'Earlier-candidate live experiments used market 6667468 and correctly refused maker admission on one-sided books; those recordings remain under experiments_final/ and rc2_summary/. The old market 6667467 was rejected before collection because it had closed and changed tick. A fresh two-sided REST preflight selected market **682705**, condition/assets in live_selection_balanced/, “Will Benjamin Netanyahu be the next Prime Minister of Israel?”. The final RC3 capture used that same explicitly rechecked market. It was a public-feed paper probe, not a new settlement-pair approval.',
      '| New public Polymarket experiment | Actual seconds | WS market frames | Native epochs | Sampled HEALTHY / DEGRADED seconds |', '|---|---:|---:|---|---|']
    for name,h in health.items():text.append(f"| {name} | {h['actual_seconds']:.6f} | {h['ws_market_frames']} | {h['epochs']} | {h['sampled_health_seconds'].get('HEALTHY',0):.3f} / {h['sampled_health_seconds'].get('DEGRADED',0):.3f} |")
    text += [f"Final RC3 balanced PAPER created {next(r['maker_orders'] for r in runs if r['profile']=='live_balanced_paper')} simulated OMS orders; conservative live fills were zero. SHADOW made no adapter call or fill. Both used bounded 180-second windows and controlled reconnect. Exact snapshots/deltas, raw hashes, availability times and sampled admission are retained. No new Kalshi authentication is claimed: its prior V0.2.0 real-data evidence is distinct. Public maker/market fee assumptions bind to frozen metadata; see the [official fee reference](https://docs.polymarket.com/trading/fees).",
      '## H. Performance',
      'Same immutable 599.796-second input, sequential native runs, optional frame logging disabled; this includes actual resulting journal/quote work. Consumer transaction latency is not network-to-commit latency. Percentiles are ms; p99.9 requires at least 10,000 samples.',
      '| Runtime | Inputs | Consumer p50 / p95 / p99 / p99.9 ms |', '|---|---:|---|']
    for p in performance:text.append(f"| {p['label']} | {p['latency']['end_to_end']['samples']} | {latency_row(p['latency']['end_to_end'])} |")
    live=next(r for r in runs if r['profile']=='live_balanced_paper')
    text += ['Balanced live PAPER stage measurements (ms; absent fill-stage samples are not zero):',
        '| Stage | Samples | p50 / p95 / p99 / p99.9 ms |','|---|---:|---|']
    for stage in ['decode_to_book_state','maker_callback','maker_request_to_risk_admission','risk_reservation_to_durable_plan','maker_durable_plan_to_adapter','oms_transition','simulated_report_to_ledger','journal_fsync','dashboard_publication','end_to_end']:
        stats=live['latency'].get(stage,{'samples':0});text.append(f"| {stage} | {stats['samples']} | {latency_row(stats)} |")
    text += ['The measured cost includes typed request auditing and actual maker plan fsync; no additional production threads/lock-free framework were added. Stage distributions and sample counts remain in runtime_latency.json and performance_comparison.json. Decode-to-state, maker callback, request-to-risk, risk-reservation-to-plan, durable-plan-to-adapter, OMS transition, report-to-ledger, journal fsync and publication are measured separately. Missing live fill-stage samples are not zero. The legacy book-update-to-strategy interval includes decoder work; legacy ws_receive_to_decode pools venue callbacks including REST/control. Historical callback-to-now series are not live latency. Do not sum percentile stages or infer calibrated one-way network delay.',
      '## I. Testing and faults',
      f"Full CTest: {tests['registered']} registered; {tests['executed']} executed; {tests['passed']} passed; {tests['failed']} known pre-existing failed; {tests['disabled']} disabled. Failure: AvalancheSkewsQuotesAndRiskCutsBuyChannel — reducing ask below fair. New maker suite: {tests['native_maker_scenarios']} scenarios passed against the frozen C++ executable; old foundation/operational tests remain. Frontend TypeScript/Vite and readonly endpoint checks passed. No new unresolved regression is hidden.",
      'Faults include cash/inventory competition, submit/ACK unknown, duplicate quote/cancel/ACK/fill, partial fills, late fills during/after cancel, cancel reject/timeout, replacement funding, crossing/tick/size, stale/suspend/reconnect, strategy error/slow quarantine, journal full/partial write, prepared/submitted/receipt crashes, incomplete restart, per-strategy request namespaces, limit reduction, kill/account/reservation mismatch, unverified settlement refusal, SHADOW forced-adapter rejection and LIVE refusal. Exact commands/expected return codes are in fault_evidence_rc3/.',
      f"Frozen native executable SHA-256: `{sha(binary)}`; source tree `{build['source_tree_sha256']}`. Fresh independent rebuild matched byte-for-byte. Five review patches apply against the preserved prechange source without staging or commits. Final analysis/UI overlays are separately identified; all native runtime files still match the tested freeze.",
      '## J. Readiness decision',
      '**PARTIALLY_VERIFIED**. Gate A architecture: PASS for existing combinatorial plus generic maker. Gate B tested paper financial invariants: PASS. Gate C tested local OMS/recovery: PASS with cooperative callback-budget and same-host/version boundaries. Gate D research integrity: PARTIAL because full specialized old-runner/product-provider parity and calibrated queue/fill probabilities remain unverified. Gate E deployment safety: PASS for no-live, simulated-only scope.',
      'Continuous L2 correctness remains limited by prior Polymarket reconstruction incidents and finite sampled validation. Three-minute probes do not replace a long soak. Callback quarantine occurs after a callback returns and cannot preempt a permanently blocked native function; wall-budget breaches under different host loads require explicit replay review. Private account/settlement authority, multi-wallet collateral and cross-host/software-upgrade recovery are not implemented. Existing maker regression remains open.',
      'Do not proceed directly to V0.2.2 cross-venue execution. First close product-runner parity, feed-integrity and supervised recovery gates, then extend this same OMS/ReservationBook to approved pairs and funded wallets. Read-only pair research can continue without enabling orders.',
      '## Reproduce and inspect',
      '```bash\ncmake -S . -B build\ncmake --build build -j4\nctest --test-dir build --output-on-failure -j4\n# Expected known unrelated maker failure is documented above.\npython3 tests/integration/runtime/unified_maker_v021.py --binary build/trading_runtime --source .\nbuild/trading_runtime --config config/trading_runtime.multi_strategy.json\n# Fresh output directory required; --recover validates the existing frozen run.\n```',
      'For the completed operations snapshot, use build/paper_backend --dashboard-file runs/runtime_v0_2_1_20261106/experiments_rc3/live_balanced/paper/dashboard.original.json --host 127.0.0.1 --port 8080; then cd frontend/dashboard && npm run dev and open http://localhost:5173/?view=runtime. It intentionally displays STOPPED. Use original snapshots for observed capture metrics; offline recovery snapshots are separate.',
      'Source/readiness contracts: docs/unified_maker_oms_v021.md, portfolio_risk_authority_v021.md, maker_cancel_replace_protocol.md, multi_strategy_runtime.md, maker_parity_report.md, maker_paper_execution_assumptions.md. audit_manifest.json binds the required artifacts, actual profiles, source/binary identities, raw inputs, native journals, CSVs, tests and analysis overlay. No indefinitely running process, real order, real fill receipt or 24/72-hour claim exists.']
    (OUT/'final_report.md').write_text('\n\n'.join(text).replace('|\n\n|','|\n|')+'\n')
    save(OUT/'acceptance_gates.json',dict(decision='PARTIALLY_VERIFIED',gates={'A':'PASS_GENERIC_SCOPE','B':'PASS_TESTED_PAPER','C':'PASS_TESTED_LOCAL_FAILURES','D':'PARTIALLY_VERIFIED_PRODUCT_RUNNER_PARITY','E':'PASS_NO_LIVE'},proceed_to_v022_execution=False,production_ready=False))
def main():
    health=live_health();report(health);changes()
    print('Final report and review patches generated; inventory follows final docs/log copies.')
if __name__=='__main__':main()

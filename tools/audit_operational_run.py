#!/usr/bin/env python3
"""Independent journal/accounting audit and same-executable live/replay comparison."""
import argparse,hashlib,json,math,os,subprocess
from pathlib import Path
from datetime import datetime,timezone
from zoneinfo import ZoneInfo

def compact(v):return json.dumps(v,separators=(',',':'),ensure_ascii=False)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def journal(path):
    previous='0'*64;events=[]
    raw=path.read_bytes()
    if raw and not raw.endswith(b'\n'):raise RuntimeError('torn journal')
    for n,line in enumerate(raw.splitlines(),1):
        row=json.loads(line);expected=hashlib.sha256((previous+'\n'+str(n)+'\n'+compact(row['event'])).encode()).hexdigest()
        if row['seq']!=n or row['prev']!=previous or row['sha256']!=expected:raise RuntimeError('journal integrity mismatch')
        previous=expected;events.append(row['event'])
    return events,previous

def main():
    p=argparse.ArgumentParser();p.add_argument('--run',required=True);p.add_argument('--binary',required=True);p.add_argument('--with-replay',action='store_true');p.add_argument('--replay-output');a=p.parse_args()
    root=Path(a.run).resolve();binary=Path(a.binary).resolve();events,head=journal(root/'events.jsonl');raw,rawhead=journal(root/'inputs.jsonl')
    cfg=json.loads((root/'frozen_config.json').read_text());dashboard=json.loads((root/'dashboard.json').read_text());runtime=dashboard['runtime']
    if cfg['live_enabled'] or cfg['mode'] not in ['PAPER','SHADOW']:raise RuntimeError('not a no-live operational run')
    if events[0]['binary_sha256']!=sha(binary):raise RuntimeError('audit binary differs from actual run')
    commits=[];transaction=None;trace=''
    for e in events[1:]:
        t=e['type']
        if t=='INPUT_BEGIN':
            if transaction is not None:raise RuntimeError('nested transaction')
            transaction=e['input_index'];trace=''
            if hashlib.sha256(compact(raw[transaction]).encode()).hexdigest()!=e['input_sha256']:raise RuntimeError('raw identity mismatch')
        elif t=='INPUT_COMMIT':
            if transaction!=e['input_index'] or hashlib.sha256(trace.encode()).hexdigest()!=e['events_sha256']:raise RuntimeError('transaction trace mismatch')
            commits.append(e);transaction=None
        elif t not in ['RUNTIME_STARTED','RUNTIME_STOPPED']:
            if transaction is None:raise RuntimeError('unscoped journal event')
            trace+=compact(e)+'\n'
    if transaction is not None:raise RuntimeError('unresolved transaction; audit cannot certify terminal accounting')
    if len(commits)!=len(raw):raise RuntimeError('completed audit requires every recorded input committed')
    transport={};messages={};epochs={};silence={};last={}
    for row in raw:
        venue=row.get('venue');wire=row.get('transport');kind=row.get('classification')
        if not venue:continue
        transport[venue+':'+str(wire)+':'+str(kind)]=transport.get(venue+':'+str(wire)+':'+str(kind),0)+1
        if wire=='ws' and kind=='market_data':
            stamp=row['recv_ns'];silence[venue]=max(silence.get(venue,0),stamp-last.get(venue,stamp));last[venue]=stamp
            epochs.setdefault(venue,set()).add(row.get('connection_epoch'))
            try:packet=json.loads(row['raw_payload'])
            except ValueError:continue
            for item in packet if isinstance(packet,list) else [packet]:
                if isinstance(item,dict):
                    t=item.get('event_type',item.get('type','unknown'));key=venue+':'+str(t);messages[key]=messages.get(key,0)+1
    fills=[e for e in events if e['type']=='EXECUTION_REPORT' and e.get('filled_lots',0)>0]
    if any(e.get('fill_source')!='SIMULATED_PAPER' or not e.get('fill_id','').startswith('SIMULATED:') for e in fills):raise RuntimeError('unexpected fill authority')
    if cfg['mode']=='SHADOW' and fills:raise RuntimeError('shadow produced executed fills')
    if cfg['mode']=='SHADOW' and any(e['type'] in ['ADAPTER_INVOKED','EXECUTION_REPORT'] for e in events):raise RuntimeError('shadow invoked execution')
    histories=[]
    history_path=root/'health_history.jsonl'
    if history_path.exists():histories=[json.loads(x) for x in history_path.read_text().splitlines()]
    durations={}
    for before,after in zip(histories,histories[1:]):
        delta=max(0,after['logical_now_ns']-before['logical_now_ns'])/1e9
        durations[before['health']]=durations.get(before['health'],0)+delta
    manifest=json.loads((root/'recording_manifest.json').read_text()) if (root/'recording_manifest.json').exists() else {}
    latency=json.loads((root/'latency.json').read_text())
    original_profile=root.parent/(root.name+'.json')
    original_config_verified=original_profile.exists() and sha(original_profile)==events[0]['config_sha256']
    metadata_path=Path(cfg['markets_file']);metadata=json.loads(metadata_path.read_text())
    identities=[]
    for item in metadata:
        raw_market=item['raw'];identities.append({'venue':item['venue'],
            'market_id':raw_market.get('ticker',raw_market.get('conditionId')),
            'title':raw_market.get('title',raw_market.get('question')),
            'outcome_assets':raw_market.get('clobTokenIds',['YES','NO'])})
    bounds_lower=[];bounds_upper=[]
    for n,row in enumerate(raw[:-1]):
        callback=row.get('callback_recv_monotonic_ns')
        if row.get('transport')=='ws' and row.get('classification')=='market_data' and callback:
            bounds_lower.append(max(0,row['recv_ns']-callback))
            bounds_upper.append(max(0,raw[n+1]['recv_ns']-callback))
    def quantiles(values):
        values=sorted(values);out={'samples':len(values),'unit':'ns'}
        for label,q,minimum in [('p50',.5,2),('p95',.95,20),('p99',.99,100),('p99_9',.999,10000)]:
            out[label]=values[math.ceil(q*len(values))-1] if len(values)>=minimum else None
        return out
    stage_names=['ws_receive_to_decode','decode_to_book_state','book_update_to_strategy','strategy_to_risk',
                 'risk_to_durable_plan','durable_plan_to_adapter','journal_write','journal_fsync','dashboard_publication','end_to_end']
    for stage in stage_names:
        latency.setdefault(stage,dict(samples=0,observed=0,unit='ns',p50=None,p95=None,p99=None,p99_9=None,status='NOT_OBSERVED_IN_THIS_RUN'))
    result={'mode':cfg['mode'],'release':'0.2.0','binary_sha256':sha(binary),'config_sha256':events[0]['config_sha256'],'frozen_config_snapshot_sha256':sha(root/'frozen_config.json'),
        'original_config_bytes_verified':original_config_verified,'market_metadata_file_sha256':sha(metadata_path),'markets':identities,
        'analysis_tool_sha256':sha(Path(__file__)),
        'recording_sha256':sha(root/'inputs.jsonl'),'journal_head':head,'raw_journal_head':rawhead,
        'actual_duration_seconds':manifest.get('actual_elapsed_ns',0)/1e9,'requested_seconds':cfg.get('duration_seconds'),
        'processed_inputs':len(commits),'recorded_inputs':len(raw),'journal_verified':True,'unresolved_transactions':0,
        'transport_counts':transport,'wire_message_counts':messages,'epochs_with_market_data':{k:sorted(v) for k,v in epochs.items()},
        'maximum_market_message_silence_ns':silence,'message_loss_note':'Silence is not proof of message loss; PM public feed has no exchange sequence.',
        'sampled_health_seconds':durations,'health_sampling_note':'approximately 1 Hz derived snapshots, includes startup/shutdown; not exact L2 coverage',
        'strategy_callbacks':runtime.get('strategy_callbacks'),'risk_decisions':sum(e['type']=='RISK_DECISION' for e in events),
        'risk_approvals':runtime.get('approvals'),'paper_submissions':runtime.get('paper_submissions'),'simulated_fill_receipts':len(fills),
        'account':dashboard['account'],'positions':runtime['positions'],'reserved_cash_tick':runtime['reserved_cost_tick'],
        'available_cash_tick':runtime.get('available_cash_tick'),'fees_paid_tick':runtime.get('fees_paid_tick'),
        'marked_unrealized_pnl_tick':dashboard['account']['unrealized_pnl_tick'],'simulated_realized_pnl_tick':dashboard['account']['realized_pnl_tick'],
        'verified_live_realized_pnl':None,'exchange_account_reconciled':False,'private_reconciliation':'UNCONFIGURED',
        'latency':latency,
        'latency_scope_notes':{'end_to_end':'consumer transaction processing; excludes ingress queue/raw persistence and dashboard',
            'book_update_to_strategy':'inclusive decoder-entry-to-callback interval; overlaps decode_to_book_state; not additive',
            'callback_to_commit_bounds':'single-writer commit lies between current and next sequencer availability; upper bound includes idle/telemetry'},
        'callback_to_commit_quantile_lower_bounds':quantiles(bounds_lower),'callback_to_commit_quantile_upper_bounds':quantiles(bounds_upper),
        'replay_comparison':'NOT_RUN','real_order_submission':'DISABLED',
        'acceptance_scope':'market_data_acceptance is receipt plus replay parity only; does not certify continuous correct L2 or production readiness',
        'kalshi_authenticated_market_data_observed':messages.get('kalshi:orderbook_snapshot',0)>0,
        'kalshi_deltas_observed':messages.get('kalshi:orderbook_delta',0)>0,
        'polymarket_books_observed':messages.get('polymarket_international:book',0)>0,
        'polymarket_deltas_observed':messages.get('polymarket_international:price_change',0)>0}
    if a.with_replay:
        replay=Path(a.replay_output).resolve() if a.replay_output else root/'replay_validation'
        if a.replay_output and replay.exists():raise RuntimeError('explicit replay output must be new; existing evidence is preserved')
        repeat=0
        while replay.exists():
            repeat+=1;replay=root/f'replay_validation_repeat_{repeat:03d}'
        config=dict(cfg,mode='REPLAY',replay_execution_policy=cfg['mode'],input_format='venue_journal',input=str(root/'inputs.jsonl'),output_dir=str(replay))
        replay_config=root/(replay.name+'.json');replay_config.write_text(json.dumps(config,indent=2)+'\n')
        env={k:v for k,v in os.environ.items() if not k.startswith('KALSHI_')}
        with (root/(replay.name+'.log')).open('w') as log:
            done=subprocess.run([str(binary),'--config',str(replay_config)],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1200)
        if done.returncode!=0:raise RuntimeError('offline replay failed; inspect the replay output log')
        replay_events,_=journal(replay/'events.jsonl');replay_commits=[e for e in replay_events if e['type']=='INPUT_COMMIT']
        comparison={'live_transactions':len(commits),'replay_transactions':len(replay_commits),
                    'decision_traces_identical':[e['events_sha256'] for e in commits]==[e['events_sha256'] for e in replay_commits],
                    'accounting_digests_identical':[e['account_sha256'] for e in commits]==[e['account_sha256'] for e in replay_commits],
                    'replay_credentials_removed':True,'replay_directory':str(replay)}
        (root/'replay_comparison.json').write_text(json.dumps(comparison,indent=2)+'\n')
        result['replay_comparison']='PASS' if comparison['decision_traces_identical'] and comparison['accounting_digests_identical'] else 'FAIL'
    result['market_data_acceptance']=all(result[k] for k in ['kalshi_authenticated_market_data_observed','kalshi_deltas_observed','polymarket_books_observed','polymarket_deltas_observed']) and result['replay_comparison']=='PASS'
    (root/'operational_audit.json').write_text(json.dumps(result,indent=2)+'\n')
    rows=['# Actual operational validation',f"Mode: {cfg['mode']}; release 0.2.0; observed {result['actual_duration_seconds']:.3f} seconds.",
          f"Market-data acceptance: {result['market_data_acceptance']}; replay: {result['replay_comparison']}.",
          'Markets: '+compact(identities),
          'Latency scope: end_to_end is consumer transaction processing; ingress delay is separate. book_update_to_strategy includes decode and is not additive. No combined percentile is invented.',
          f"Strategy callbacks: {result['strategy_callbacks']}; risk decisions: {result['risk_decisions']}; simulated fill receipts: {len(fills)}.",
          'No exchange orders. Verified live realized PnL is null. Private account reconciliation is unconfigured.',
          '\n| Latency (ns) | Samples | p50 | p95 | p99 | p99.9 |','|---|---:|---:|---:|---:|---:|']
    for name,values in latency.items():rows.append('| '+name+' | '+' | '.join(str(values.get(x)) for x in ['samples','p50','p95','p99','p99_9'])+' |')
    (root/'operational_report.md').write_text('\n\n'.join(rows).replace('|\n\n|','|\n|')+'\n')
    if result['replay_comparison']=='FAIL':raise RuntimeError('decision/account replay comparison failed')
    print(json.dumps({k:result[k] for k in ['mode','actual_duration_seconds','strategy_callbacks','risk_decisions','simulated_fill_receipts','market_data_acceptance','replay_comparison']}))
if __name__=='__main__':main()

"""Report real native authentication/capture evidence, independent of pair approval."""
import argparse
import hashlib
import json
from pathlib import Path


def build(workflow,out):
    if out.exists() and any(out.iterdir()):raise ValueError('report output must be new/empty')
    out.mkdir(parents=True,exist_ok=True)
    capture=workflow/'capture';audit=workflow/'capture_feed_audit_verified' if (workflow/'capture_feed_audit_verified').exists() else workflow/'capture_feed_audit'
    manifest=json.loads((capture/'capture_manifest.json').read_text())
    health=json.loads((audit/'summary.json').read_text())
    analysis=capture/'analysis_verified' if (capture/'analysis_verified').exists() else capture/'analysis'
    replay=json.loads((analysis/'summary.json').read_text())
    key=json.loads((workflow/'key_check.json').read_text())
    kalshi=health['wire_protocol'].get('kalshi',{})
    poly=health['wire_protocol'].get('polymarket_international',{})
    observed=bool(kalshi.get('snapshot_packets',0) and kalshi.get('delta_packets',0))
    counts=health['statistics']['counts']
    valid=observed and counts.get('kalshi:valid_book_observation',0)>0
    simultaneous=health['both_venues_any_asset_healthy_l2_sec']
    duration=manifest['actual_duration_sec']
    transport_verified=valid and duration>=600 and simultaneous>0 and poly.get('snapshot_packets',0)>0
    events=[json.loads(line) for line in (capture/'connection_events.jsonl').read_text().splitlines()]
    controlled=[]
    for event in events:
        if event['classification']!='validation_reconnect':continue
        rows=[r for r in health['snapshot_recovery'] if r['venue']==event['venue'] and r['connection_epoch']==event['connection_epoch']+1]
        controlled.append({'venue':event['venue'],'request_epoch':event['connection_epoch'],'request_utc':event['recv_utc'],
            'next_epoch_markets_with_snapshot':sum(bool(r['first_snapshot_record']) for r in rows),
            'next_epoch_markets_with_snapshot_then_delta':sum(r['snapshot_then_delta_verified'] for r in rows),
            'scope':'Kalshi rows represent both outcome books per market; Polymarket rows represent individual outcome assets'})
    summary={'schema':'cross-venue-authenticated-l2-validation-v0.1.1',
        'kalshi_authentication_blocker_removed':valid,'bounded_dual_ws_validation_complete':transport_verified,
        'project_verification_status':'PARTIALLY_VERIFIED_ECONOMIC_APPROVAL_NOT_GRANTED',
        'start_utc':manifest['start_utc'],'end_utc':manifest['end_utc'],'actual_duration_sec':duration,
        'raw_records':health['raw_records_verified'],'raw_bytes':manifest['raw_bytes'],'fatal_errors':manifest['fatal_errors'],
        'key_algorithm':key['algorithm'],'credentials_recorded':False,'wire_protocol':health['wire_protocol'],
        'connections':health['connections'],'both_venues_any_asset_healthy_l2_sec':simultaneous,
        'kalshi_all_selected_assets_healthy_l2_sec':health['kalshi_all_selected_assets_healthy_l2_sec'],
        'polymarket_all_selected_assets_healthy_l2_sec':health['polymarket_all_selected_assets_healthy_l2_sec'],
        'diagnostic_watch_pairs':health['diagnostic_watch_pairs'],'approved_continuous_synchronized_l2_sec':health['approved_continuous_synchronized_l2_sec'],
        'controlled_recovery':controlled,'snapshot_recovery':health['snapshot_recovery'],
        'reconstruction_rejections':health['statistics']['rejections'],
        'snapshot_reconciliation_mismatches':health['snapshot_reconciliation_mismatches'],
        'inconclusive_comparisons':health['inconclusive_comparisons'],'local_record_sequence_gaps':health['local_record_sequence_gaps'],
        'replay_verdict':replay['verdict'],'approved_pairs':replay['approved_pairs'],
        'latency_execution_scenarios':len(replay['latency_scenarios']),'simulated_trades':replay['simulated_trades_across_scenarios'],
        'live_orders_sent':manifest['live_orders_sent'],'verified_live_realized_pnl':None,
        'scope':'Authenticated L2 transport/state evidence does not approve settlement-rule equivalence or demonstrate executable arbitrage.'}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    lines=['# Authenticated Kalshi × Polymarket International L2 validation',
        f"Kalshi authentication blocker removed: **{valid}**. Completed bounded dual-WS validation: **{transport_verified}**.",
        f"Actual capture: {manifest['start_utc']} through {manifest['end_utc']} ({duration:.3f} seconds).",
        'No real orders. No credential values or signatures stored. `verified_live_realized_pnl = null`.',
        '|Venue|WS snapshots|WS deltas|Sequence-gap events|Missing sid/seq|Clock regressions|Delta before snapshot|',
        '|---|---:|---:|---:|---:|---:|---:|']
    for venue,w in health['wire_protocol'].items():
        seq=[w.get(k,0) for k in ['snapshot_packets','delta_packets','subscription_sequence_gap_events','missing_sid_seq_packets','clock_regressions_within_session_market','deltas_before_snapshot']]
        lines.append('|'+venue+'|'+'|'.join(map(str,seq))+'|')
    lines += ['', 'CLOB has no contiguous public exchange sequence; its zero local gap-event counter does not establish exchange completeness.',
        '## Connection and reconstruction health',json.dumps(health['connections'],indent=2),
        f"Simultaneous healthy WS coverage (at least one asset on each venue): {simultaneous:.3f} seconds.",
        f"Kalshi all selected assets healthy: {health['kalshi_all_selected_assets_healthy_l2_sec']:.3f} seconds; Polymarket: {health['polymarket_all_selected_assets_healthy_l2_sec']:.3f} seconds.",
        'Unapproved diagnostic watch coverage: '+json.dumps(health['diagnostic_watch_pairs'],indent=2),
        'Approved economic-pair coverage remains separately reported; diagnostic watches never grant approval.',
        '## Timestamp age and local serialization',
        '|Venue|Book source→callback p50 ms|p95 ms|max ms|Negative samples|Local callback→processing p95 ms|',
        '|---|---:|---:|---:|---:|---:|']
    for venue,w in health['wire_protocol'].items():
        age=w.get('book_source_to_callback_age_ms',w['source_to_callback_age_ms']);local=w['callback_to_processing_ms']
        lines.append('|'+venue+'|'+'|'.join(str(v) for v in [age['p50'],age['p95'],age['max'],age['negative_samples'],local['p95']])+'|')
    lines += ['Source age includes exchange/local clock offset, batching and transit; it is not measured network RTT. Negative samples are retained.',
        '## Recovery',json.dumps(controlled,indent=2),
        'See native snapshot_recovery.csv for each market/epoch initial snapshot and subsequent delta record.',
        '## Existing five-latency/four-model replay',
        f"{len(replay['latency_scenarios'])} scenarios completed. Verdict {replay['verdict']}; approved pairs {replay['approved_pairs']}; simulated trades {replay['simulated_trades_across_scenarios']}.",
        'Market-rule, quantity, fee and settlement-asset gates remain active. Authentication success is not evidence of profitable or approved trading.',
        '## Remaining feed failures',json.dumps(health['statistics']['rejections'],indent=2)]
    (out/'report.md').write_text('\n\n'.join(lines).replace('|\n\n|','|\n|')+'\n')
    inputs=[]
    for path in [workflow/'key_check.json',capture/'capture_manifest.json',analysis/'summary.json',analysis/'audit_manifest.json',audit/'summary.json',audit/'audit_manifest.json',Path(__file__)]:
        inputs.append({'file':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    (out/'audit_manifest.json').write_text(json.dumps({'inputs':inputs,'credentials_recorded':False},indent=2)+'\n')
    return summary


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workflow-dir',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();s=build(a.workflow_dir,a.out)
    print(json.dumps({k:s[k] for k in ['kalshi_authentication_blocker_removed','bounded_dual_ws_validation_complete','actual_duration_sec','raw_records','verified_live_realized_pnl']},indent=2))

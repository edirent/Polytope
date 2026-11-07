"""Join immutable review, feed and replay evidence without inventing approval."""
import argparse
import hashlib
import json
from pathlib import Path


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def build(source,review,capture,feed,out):
    if out.exists() and any(out.iterdir()):raise ValueError('report output must be new/empty')
    out.mkdir(parents=True,exist_ok=True)
    original=json.loads((source/'capture_manifest.json').read_text())
    manifest=json.loads((review/'approval_manifest.pending.json').read_text())
    assert sha(source/'candidate_pairs.json')==manifest['source_candidate_sha256']
    assert sha(source/'market_metadata.json')==manifest['source_metadata_sha256']
    review_summary=json.loads((review/'summary.json').read_text())
    analysis=capture/'analysis_v0_1_1' if (capture/'analysis_v0_1_1').exists() else capture/'analysis'
    result=json.loads((analysis/'summary.json').read_text())
    health=json.loads((feed/'summary.json').read_text())
    funnel=[{'stage':'Discovered Markets','count':sum(original['market_counts'].values()),'scope':'original reviewed discovery sample'},
        {'stage':'Candidate Pairs','count':original['candidate_pairs'],'scope':'all frozen candidates'},
        {'stage':'Reviewed Pairs','count':review_summary['top_dossiers'],'scope':'detailed source dossiers; automated triage covers all 200; no human decision inferred'},
        {'stage':'Approved Pairs','count':result['approved_pairs'],'scope':'explicit human-approved native artifacts'},
        {'stage':'Healthy Synchronized Observations','count':result['healthy_synchronized_observations'],'scope':'approved WS+WS only; excludes all REST-only state'},
        {'stage':'Gross Positive Opportunities','count':result['positive_gross_observations'],'scope':'approved analysis universe only'},
        {'stage':'Net Positive Opportunities','count':result['positive_net_observations'],'scope':'known fees, depth, assets, rules and constraints required'},
        {'stage':'Latency-Surviving Opportunities','count':sum(r['survived'] for r in result['latency_scenarios']),'scope':'scenario memberships, not independent liquidity'},
        {'stage':'Simulated Trades','count':result['simulated_trades_across_scenarios'],'scope':'scenario memberships, not exchange fills'}]
    summary={'schema':'cross-venue-v0.1.1-evidence-report','verification_status':'PARTIALLY_VERIFIED',
        'acceptance_met':result['approved_pairs']>0 and health['approved_continuous_synchronized_l2_sec']>0,
        'verdict':result['verdict'],'funnel':funnel,'human_review_decisions':review_summary['human_reviewed_pairs'],
        'healthy_approved_synchronized_seconds':health['approved_continuous_synchronized_l2_sec'],
        'reviewed_rule_compatibility':{k:review_summary[k] for k in ['potentially_related_needs_human_review','contradictory_or_unrelated']},
        'simulated_net_pnl':None if not result['approved_pairs'] else 'SEE_PER_SCENARIO_CSV',
        'opportunity_duration':None,'capacity':result['opportunity_capacity_upper_bound_contracts'],
        'maximum_capital_utilization':max(r['max_capital_utilization'] for r in result['latency_scenarios']),
        'primary_blockers':['NO_EXPLICIT_HUMAN_APPROVAL','MISSING_KALSHI_WS_AUTHORIZATION',
            'QUANTITY_AND_SETTLEMENT_ASSET_UNVERIFIED','CROSS_ASSET_FX_DEPEG_MODEL_NOT_IMPLEMENTED'],
        'rule_rejection_counts':review_summary['blocker_counts'],'feed_audit':health,
        'latency_scenarios':result['latency_scenarios'],'verified_live_realized_pnl':None,'live_orders_sent':0,
        'long_capture_status':'NOT_STARTED_PREREQUISITES_MISSING; 24h/72h bounded workflows provided',
        'scope_note':'Review funnel uses the original frozen queue; feed diagnostics use a fresh capture of the same eight market identities. No cross-run observations or profits are combined.'}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    lines=['# Polytope V0.1.1 — evidence and blockers',
        'Status **PARTIALLY_VERIFIED**; acceptance **not met** unless approved continuous L2 evidence is present.',
        'No real orders. `verified_live_realized_pnl = null`.',
        '|Funnel stage|Count|Scope|','|---|---:|---|']
    lines += [f"|{r['stage']}|{r['count']}|{r['scope']}|" for r in funnel]
    lines += ['',summary['scope_note'],
        'A zero approved universe is not evidence of zero market-wide arbitrage. PnL, capacity and opportunity duration remain unmeasured.',
        '## Feed evidence',json.dumps(health,indent=2),
        '## Five latencies × four execution models',
        '|Latency ms|Model|Valid|Survival|Eligible capital|Trades|Gross|Fees|Modeled loss|Net|Residual|Max utilization|',
        '|---:|---|---:|---|---:|---:|---|---|---|---|---|---:|']
    def display(row,key):return 'UNMEASURED' if row[key] is None else str(row[key])
    for r in result['latency_scenarios']:
        lines.append('|'+ '|'.join(display(r,k) for k in ['latency_ms','execution_model','valid_opportunities','survival_rate','eligible_capital','simulated_trades','simulated_gross_pnl','fees','modeled_execution_loss','simulated_net_pnl','max_residual_exposure','max_capital_utilization'])+'|')
    lines += ['## Next required evidence','\n'.join('- '+b for b in summary['primary_blockers']),
        'Human review must resolve rule differences, pin actual settlement assets and quantity steps, and approve a specific revision. A chat-level blanket approval must not erase contradictory payoff states.',
        'The 24-hour research campaign remains blocked. A 10-minute read-only feed stability diagnostic is not a 24-hour approved-pair backtest. No 72-hour job has been started.']
    (out/'report.md').write_text('\n\n'.join(lines)+'\n')
    inputs=[]
    for directory,names in [(source,['capture_manifest.json','candidate_pairs.json','market_metadata.json']),
            (review,['audit_manifest.json','summary.json','approval_manifest.pending.json']),
            (capture,['capture_manifest.json',str(analysis.relative_to(capture)/'summary.json'),str(analysis.relative_to(capture)/'audit_manifest.json')]),
            (feed,['summary.json','diagnostics.json','audit_manifest.json'])]:
        for name in names:inputs.append({'file':str(directory/name),'sha256':sha(directory/name)})
    (out/'audit_manifest.json').write_text(json.dumps({'inputs':inputs,'report_script_sha256':sha(Path(__file__))},indent=2)+'\n')
    return summary


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['source-run','review-dir','capture-dir','feed-audit-dir','out']:p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();s=build(a.source_run,a.review_dir,a.capture_dir,a.feed_audit_dir,a.out)
    print(json.dumps({k:s[k] for k in ['verification_status','acceptance_met','verdict','funnel','verified_live_realized_pnl']},indent=2))

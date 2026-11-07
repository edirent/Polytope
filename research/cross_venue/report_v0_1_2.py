"""Assemble native economic evidence; arithmetic summaries only, no pricing engine."""
import argparse,csv,hashlib,json,shutil
from collections import Counter,defaultdict
from decimal import Decimal as D
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
FILES=['asof_coverage_comparison.csv','joint_book_health.csv','candidate_pair_diagnostics.csv','price_dislocations.csv','opportunity_episodes.csv','fee_depth_analysis.csv','latency_sensitivity.csv','maker_quote_feasibility.csv','capital_feasibility.csv','maker_quote_episodes.csv','maker_safety_sensitivity.csv']
CHANGES={
 'engine/state/cross_venue/JointBookAsOf.h':'Value view and explicit availability/transport/integrity/authority/tradability flags; no new state engine',
 'engine/state/cross_venue/JointBookAsOf.cpp':'Policy-specific monotonic As-Of checks; quiet states require validation at or after their last change; research policy cannot authorize strategy',
 'engine/state/cross_venue/Types.h':'Policy fields and stable content/actual-change provenance; legacy defaults preserved',
 'engine/state/cross_venue/SynchronizedBookStore.h':'Opt-in representation of valid empty/one-sided real L2',
 'engine/state/cross_venue/SynchronizedBookStore.cpp':'Stable content hash, actual change time, epoch validation and As-Of dispatch; legacy skew stays default',
 'engine/common/math/FractionalVwap.h':'Exact micro-share depth walk with int128 monetary products',
 'engine/decode/parser/kalshi/KalshiBookNormalizer.cpp':'Omitted empty WS side allowed only in fractional real protocol; strict legacy mode unchanged',
 'engine/strategy/cross_venue/LiveResearch.h':'Existing research interfaces extended for diagnostics, fees and transport evidence',
 'engine/strategy/cross_venue/ResearchState.cpp':'Expose existing WS source ownership; opt-in empty-state reconstruction; As-Of strict scan requires WS',
 'engine/strategy/cross_venue/PairScanner.cpp':'Bind enabled As-Of policy to intent proof; existing risk/ledger/execution preserved',
 'engine/strategy/cross_venue/MarketDiscovery.cpp':'Reuse frozen fee engine for exact micro-share quantities; integer interface delegates',
 'engine/feed/source_runtime/CrossVenuePublicData.cpp':'Read opt-in As-Of thresholds from existing configuration',
 'engine/feed/source_runtime/CrossVenueLiveCapture.cpp':'Hashed processing-completion sidecar; raw exchange frames/timestamps preserved',
 'replay/cross_venue/LiveL2Replay.cpp':'Validate completion-sidecar hash and sequence; make new records available only after actual processing completes',
 'engine/strategy/cross_venue/EconomicDiscovery.cpp':'Native immutable replay; policy coverage, both directions, depth/fee diagnostics, episodes, future-state latency, conditional maker and two-wallet requirements; invokes existing Oracle constraint builder for unapproved hypotheses',
 'engine/strategy/cross_venue/CMakeLists.txt':'Native diagnostic CLI and V0.1.2 tests',
 'tools/cross_venue_research_main.cpp':'Read-only diagnostic command dispatch; no order submission flag',
 'config/cross_venue_v0_1_2.json':'Nine related candidates only; explicit 20-minute launcher; existing pending approval manifest',
 'research/cross_venue/secure_kalshi_environment.py':'New bounded launch-economic action using local environment only; legacy secure actions preserved',
 'research/cross_venue/economic_capture_workflow.py':'1200-second native capture followed by finite audit and economic analysis',
 'research/cross_venue/review_v0_1_2.py':'Official-source refresh, nine pending dossiers and clock/REST-RTT diagnostics',
 'research/cross_venue/audit_v0_1_2.py':'Independent raw protocol/price-depth/arithmetic/cutoff/episode/byte-equality audit',
 'research/cross_venue/report_v0_1_2.py':'Evidence assembly, statistical summary, target arithmetic and hash manifest',
 'tests/integration/cross_venue/cross_venue_v0_1_2_test.cpp':'Native As-Of, empty/one-sided real wire regression, fractional fees, clock and approval tests',
 'tests/integration/cross_venue/v012_replay_audit.py':'Two real native historical replays and independent audit in CTest',
 'docs/cross_venue_v0_1_2.md':'Implementation, policy, evidence interpretation and reproducibility guide'}
def write(p,v):p.write_text(json.dumps(v,indent=2,ensure_ascii=False)+'\n')
def read(p):return json.loads(p.read_text())
def rows(p):return list(csv.DictReader(p.open()))
def table(p,data):
    fields=sorted(set().union(*(r.keys() for r in data))) if data else ['status','reason']
    with p.open('w',newline='') as stream:
        w=csv.DictWriter(stream,fields);w.writeheader();w.writerows(data)
def stats(a):
    if not a:return None
    a=sorted(a);return {'n':len(a),'min':str(a[0]),'p50':str(a[(len(a)-1)//2]),'p95':str(a[int((len(a)-1)*.95)]),'max':str(a[-1])}
def run(out,analyses,testlog):
    sources=[];combined={f:[] for f in FILES};native_summaries=[]
    for name,directory in analyses:
        s=read(directory/'economic_summary.json');native_summaries.append({'capture_id':name,**s});capture=Path(s['source_run']);capture=capture if capture.is_absolute() else ROOT/capture
        sources.append({'capture_id':name,'capture_directory':str(capture),'native_analysis_directory':str(directory),'capture_manifest_sha256':hashlib.sha256((capture/'capture_manifest.json').read_bytes()).hexdigest(),'manifest':read(capture/'capture_manifest.json')})
        for f in FILES:combined[f]+=[{'capture_id':name,**r} for r in rows(directory/f)]
    for f,values in combined.items():table(out/f,values)
    prices=[r for r in combined['price_dislocations.csv'] if r.get('policy')=='balanced_asof'];maker=combined['maker_quote_feasibility.csv'];episodes=[r for r in combined['opportunity_episodes.csv'] if r.get('policy')=='balanced_asof'];groups=defaultdict(list)
    for r in prices:groups[(r['capture_id'],r['candidate'],r['direction'])].append(r)
    measured=[]
    for (capture,candidate,direction),g in groups.items():
        best=max(g,key=lambda r:D(r['conditional_net_discrepancy'])/D(r['quantity']));fee_best=max(g,key=lambda r:D(r['fee_adjusted_discrepancy'])/D(r['quantity']))
        measured.append({'capture_id':capture,'candidate':candidate,'direction':direction,'priced_distinct_size_observations':len(g),'gross_per_share':stats([D(r['gross_parity_discrepancy'])/D(r['quantity']) for r in g]),'fee_adjusted_per_share':stats([D(r['fee_adjusted_discrepancy'])/D(r['quantity']) for r in g]),'budget_adjusted_per_share':stats([D(r['conditional_net_discrepancy'])/D(r['quantity']) for r in g]),'strongest_fee_adjusted_row':fee_best,'strongest_budget_adjusted_row':best,'max_tested_quantity':str(max(D(r['quantity']) for r in g)),'nominal_observed_depth':stats([D(r['visible_pair_depth']) for r in g]),'execution_capacity':None,'strict_pnl':None})
    annual=[]
    for target in [10000,25000,50000,100000]:
        for edge in [D('.001'),D('.005'),D('.01')]:annual.append({'target_profit_usd':target,'capital_usd':200000,'assumed_average_net_edge':str(edge),'required_full_capital_turnovers':str(D(target)/(D(200000)*edge)),'achievability':'NOT_ESTABLISHED','basis':'Arithmetic only; no annualization; two independent wallets and holding/market availability constraints remain'})
    table(out/'annual_target_requirements.csv',annual)
    target_edges=[{'target_usd':p,'turnovers':t,'required_average_net_edge':str(D(p)/(D(200000)*t))} for p in [10000,25000,50000,100000] for t in [1,4,12]]
    write(out/'annual_target_edges.json',target_edges)
    # Bind pending dossiers to final measured evidence; human decisions stay null.
    pending=read(out/'approval_manifest.pending.json')
    for review in pending['reviews']:
        dossier_path=out/review['dossier_file'];dossier=read(dossier_path);kid=dossier['kalshi_market']
        matched=[m for m in measured if m['candidate'].startswith(kid) or (kid=='KXALIENS-27' and m['candidate']=='aliens_candidate_pending_review')]
        dossier['book_scope']='Final synchronized WS capture evidence below; absent executable asks remain unavailable. Archived REST is separate and not continuous L2.'
        dossier['synchronized_capture_evidence']={'native_measurements':matched,'coverage_rows':[r for r in combined['asof_coverage_comparison.csv'] if r['candidate'].startswith(kid) or (kid=='KXALIENS-27' and r['candidate']=='aliens_candidate_pending_review')],'capture_manifest_hashes':[{'capture_id':s['capture_id'],'sha256':s['capture_manifest_sha256']} for s in sources]}
        dossier.pop('dossier_sha256',None);dossier['dossier_sha256']=hashlib.sha256(json.dumps(dossier,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()).hexdigest();write(dossier_path,dossier);review['dossier_sha256']=dossier['dossier_sha256']
        assert not review.get('reviewer') and not review.get('reviewed_at') and not review.get('approved_revision') and not review.get('coverage_proven')
    write(out/'approval_manifest.pending.json',pending)
    # Rank measured candidates without approving or modifying payoff restrictions.
    ranking=read(out/'pair_review_dossiers'/'ranking.json')
    for r in ranking:
        matching=[a for a in measured if a['candidate'].startswith(r['kalshi_market']) or (r['kalshi_market']=='KXALIENS-27' and a['candidate']=='aliens_candidate_pending_review')]
        r['measured_best_conditional_net_per_share']=max((D(a['budget_adjusted_per_share']['max']) for a in matching),default=None)
        if r['measured_best_conditional_net_per_share'] is not None:r['measured_best_conditional_net_per_share']=str(r['measured_best_conditional_net_per_share'])
        r['captured_price_directions']=len(matching)
    ranking.sort(key=lambda r:(r['measured_best_conditional_net_per_share'] is None,-D(r['measured_best_conditional_net_per_share'] or '-100'),r['rank']))
    for i,r in enumerate(ranking,1):r['economic_review_rank']=i
    write(out/'pair_review_dossiers'/'economic_ranking.json',ranking)
    transport_health=[]
    for source in sources:
        capture=Path(source['capture_directory']);audit_path=(out/'baseline_feed_audit'/'summary.json') if source['capture_id']=='historical' else capture.parent/'feed_audit'/'summary.json'
        if not audit_path.exists():continue
        a=read(audit_path);wire=a['wire_protocol'];events=[json.loads(line) for line in (capture/'connection_events.jsonl').open()];last_disconnect={};current_error={};recoveries=[]
        completion={}
        if source['manifest'].get('processing_timing_file'):
            completion={r['record_sequence']:r['processing_completed_monotonic_ns'] for r in (json.loads(line) for line in (capture/source['manifest']['processing_timing_file']).open())}
        for event in events:
            event['effective_availability_ns']=completion.get(event['record_sequence'],event['availability_monotonic_ns'])
            venue=event['venue'];kind=event['classification']
            if kind=='connection_error':current_error[venue]=event['raw_payload']
            if kind=='disconnect':last_disconnect[venue]=event
            if kind=='connection_open' and venue in last_disconnect:
                previous=last_disconnect.pop(venue);recoveries.append({'venue':venue,'disconnect_epoch':previous['connection_epoch'],'reopen_epoch':event['connection_epoch'],'downtime_sec':(event['effective_availability_ns']-previous['effective_availability_ns'])/1e9,'availability_basis':'PROCESSING_COMPLETION' if completion else 'ORIGINAL_SERIALIZED_RECEIVE','latest_transport_error':current_error.pop(venue,None)})
        transport_health.append({'capture_id':source['capture_id'],'native_feed_audit':str(audit_path),'raw_records_verified':a['raw_records_verified'],'connections':a['connections'],'recoveries':recoveries,'snapshot_reconciliation_mismatches':a['snapshot_reconciliation_mismatches'],'inconclusive_comparisons':a['inconclusive_comparisons'],'book_rejects':a['book_rejects'],'kalshi_wire':wire['kalshi'],'poly_wire':wire['polymarket_international'],'both_venues_any_asset_healthy_l2_sec':a['both_venues_any_asset_healthy_l2_sec'],'kalshi_snapshot_and_delta_observed':a['kalshi_snapshot_and_delta_observed'],'server_root_cause':'UNVERIFIED; TLS stream truncation is an observed transport symptom','missing_exchange_messages':None})
    write(out/'health_audit.json',transport_health)
    tests={'status':'NOT_RECORDED'}
    if testlog and testlog.exists():
        text=testlog.read_text();tests={'log':str(testlog),'known_unrelated_failure':'AvalancheSkewsQuotesAndRiskCutsBuyChannel','cross_venue_failed':any('CrossVenue' in line and ('***Failed' in line or '(Failed)' in line) for line in text.splitlines()),'summary_lines':[line for line in text.splitlines() if 'tests passed' in line or 'Total Test time' in line]}
    positive=sum(D(r['conditional_net_discrepancy'])>0 for r in prices);fee_positive=sum(D(r['fee_adjusted_discrepancy'])>0 for r in prices)
    recommendation='EXPAND_EVENT_RELATIONSHIP_SEARCH' if len(sources)>1 and positive==0 else 'INSUFFICIENT_EVIDENCE'
    sensitivity_groups=defaultdict(list)
    for r in combined['maker_safety_sensitivity.csv']:sensitivity_groups[r['safety_plus_minprofit_per_share']].append(r)
    sensitivity=[{'margin_per_share':margin,'size_state_venue_rows':len(g),'conditional_post_only_price_rows':sum(r['post_only_price_eligible']=='true' for r in g),'conditional_competitive_rows':sum(r['competitive_at_best_bid']=='true' for r in g),'fills':None,'pnl':None} for margin,g in sorted(sensitivity_groups.items())]
    summary={'schema':'cross-venue-economic-report-v0.1.2','recommendation':recommendation,'native_captures':native_summaries,'transport_health_file':'health_audit.json','aggregate_recorded_seconds_noncontiguous':sum(s['capture_seconds'] for s in native_summaries),'related_candidates_reviewed':9,'approved_pairs':0,'strict_state':'NO_APPROVED_STRICT_ARBITRAGE','strict_simulated_pnl':None,'verified_live_realized_pnl':None,'live_orders_sent':0,'diagnostic_statistics':measured,'balanced_priced_size_state_observations':len(prices),'balanced_fee_positive_observations':fee_positive,'balanced_budget_positive_observations':positive,'maker_rows':len(maker),'conditional_post_only_rows':sum(r['post_only_eligible']=='true' for r in maker),'conditional_at_best_bid_rows':sum(r['competitive_at_best_bid']=='true' for r in maker),'maker_safety_sensitivity':sensitivity,'maker_fills':None,'maker_execution_capacity':None,'gross_episodes':sum(r['basis']=='GROSS_PARITY' for r in episodes),'fee_positive_episodes':sum(r['basis']=='FEE_ADJUSTED' for r in episodes),'budget_positive_episodes':sum(r['basis']=='BUDGET_ADJUSTED' for r in episodes),'episode_caveat':'Episodes per size and direction; overlapping sizes and repeated books are not independent trades; no allocation/reuse of capital modeled','annual_targets':'Arithmetic requirements only; no annualization','tests':tests,'strict_four_execution_models':'Existing strict replay and synthetic regression preserved; no approved real pair means real strict model PnL unevaluated','missing_evidence':['complete exception-state payoff proof','explicit human revision approval','actual market collateral identity/redemption/conversion treatment','verified quantity step and minimum on both venues','private execution/queue/receipt data','longer stable, broad short-duration market observations']}
    write(out/'conversion_funnel.json',{'strict':{'discovered_markets_by_capture':[{'capture_id':s['capture_id'],'market_counts':s['manifest']['market_counts']} for s in sources],'candidate_pairs_by_capture':[{'capture_id':s['capture_id'],'count':s['manifest']['candidate_pairs']} for s in sources],'reviewed_related_pairs':9,'approved_pairs':0,'strict_healthy_observations':0,'strict_gross_positive_opportunities':0,'strict_net_positive_opportunities':0,'strict_latency_surviving_opportunities':0,'strict_simulated_trades':0,'strict_pnl':None},'diagnostic_only':{'priced_size_state_observations':len(prices),'fee_positive_observations':fee_positive,'budget_positive_observations':positive,'not_approved_trades':True}})
    write(out/'economic_summary.json',summary)
    failed=ROOT/'runs/cross_venue/v0_1_2_focused_20261106/capture/capture_manifest.json'
    if failed.exists():
        summary['excluded_failed_capture']={'manifest':str(failed),'sha256':hashlib.sha256(failed.read_bytes()).hexdigest(),'actual_duration_sec':read(failed)['actual_duration_sec'],'interrupted':read(failed)['interrupted'],'used_as_healthy_validation':False}
        write(out/'economic_summary.json',summary)
    write(out/'capture_manifest.json',{'schema':'cross-venue-economic-evidence-index-v0.1.2','sources':sources,'raw_inputs_immutable':True,'new_raw_frames_synthesized':False,'source_clock_correction':False,'live_orders_sent':0,'verified_live_realized_pnl':None})
    clock=read(out/'clock_diagnostics.json');clock['native_replay_diagnostics']=[{'capture_id':name,**read(directory/'clock_replay_diagnostics.json')} for name,directory in analyses];write(out/'clock_diagnostics.json',clock)
    text=['# Polytope Cross-Venue V0.1.2 — measured economic discovery','',f'Recommendation: **{recommendation}**. Strict state: **NO_APPROVED_STRICT_ARBITRAGE**. All price results are UNAPPROVED_PRICE_DIAGNOSTIC; strict simulated PnL and verified live realized PnL are null. Real order submissions: 0.','',
        '## A. Implementation','\n'.join(f'- `{p}` — {why}.' for p,why in CHANGES.items()),'',
        '## B. Real As-Of evidence','The legacy row reproduces last-update skew, while V0.1.1 strategy code separately used authoritative-time skew when configured. Both remain distinct from the new As-Of gate. Last-update skew is diagnostic in enabled V0.1.2 policies; legacy defaults and golden outputs are preserved. Coverage is diagnostic book availability, not contractual/executable arbitrage eligibility.','',
        '| Capture | Candidate | Direction | Policy | Eligible seconds | Percent |','|---|---|---|---|---:|---:|']
    for r in combined['asof_coverage_comparison.csv']:text.append(f"| {r['capture_id']} | {r['candidate']} | {r['direction']} | {r['policy']} | {float(r['eligible_sec']):.6f} | {float(r['eligible_pct']):.3f}% |")
    text+=['','Thresholds: balanced = liveness 30s / authority 45s / book change 15s; conservative = 15s / 10s / 5s; permissive research-only = 30s / 90s / 60s. Quiet books beyond the change limit require authoritative validation at or after the latest actual change. Policies reject future evidence, unresolved integrity/epoch errors, non-WS data, nontradable markets, authority expiry and unavailable asks. Exact mutually exclusive rejection seconds are in `asof_coverage_comparison.csv`; overlapping component flags are in `joint_book_health.csv`.','',
        '## C. Real diagnostic prices','Direction 0 = Kalshi YES + Polymarket NO; direction 1 = Kalshi NO + Polymarket YES. Nominal unit parity is a diagnostic reference, never a proven payout floor. All monetary inputs/calculations are integer microcurrency and microshares. Unknown quantity steps mean explicitly conditional probes, never certified orders. Visible depth is the observed best-64 prefix and is not profitable execution capacity.','',
        '| Capture | Candidate | Direction | Best gross/share | Best fee-adjusted/share | Best budget-adjusted/share | Tested max quantity |','|---|---|---:|---:|---:|---:|---:|']
    for r in measured:text.append(f"| {r['capture_id']} | {r['candidate']} | {r['direction']} | {r['gross_per_share']['max']} | {r['fee_adjusted_per_share']['max']} | {r['budget_adjusted_per_share']['max']} | {r['max_tested_quantity']} |")
    text+=['',f"Balanced priced size-state observations: {len(prices)}; fee-positive: {fee_positive}; budget-positive: {positive}. These are observations, not trades. Budget = .002/share forward deterioration plus .003/share conditional conversion cost. Observed depth slippage is already in VWAP and not charged twice.",
        'Market-specific frozen fee records are used only within their original validity period. Kalshi fee plus notional is conservatively rounded upward per level to non-direct-member cent precision; unknown rebates/grouping are not assumed. Account membership and actual receipts are absent, so these are conservative estimates. Even model fees before conservative rounding must be considered when assessing tiny gross discrepancies. Sources: [Kalshi fee rounding](https://docs.kalshi.com/getting_started/fee_rounding), [Kalshi fee schedule](https://kalshi.com/docs/kalshi-fee-schedule.pdf), [Polymarket fees](https://docs.polymarket.com/trading/fees). Current global pages do not overwrite frozen historical market-specific coefficients.','',
        'Nine dossiers include full archived rules, contract-PDF byte/text hashes, refreshed official metadata, exception-state counterexamples, asset/quantity caveats and null human decisions. Native Oracle implication compilation is a draft hypothesis only: no payoff states are excluded and no constraint is injected into the strategy. Nomination predicates broadly align in normal states but full exceptions/NegRisk/collateral remain unverifiable. Aliens source definitions/cutoff semantics prevent certifying equivalence. `pair_review_dossiers/economic_ranking.json` ranks measured candidates for further review; no ranking approves them.','',
        '## D. Latency and persistence','Five delays use the latest reconstructed state available at or before execution time, with policy reevaluation. A missing later state/depth/health is unknown. Zero positive-net observations produce an undefined/null net-survival probability, not 100% or a simulated fill. Gross-only survival is separately reported. Unchanged observations form continuous positive episodes; crossing an invalid interval closes an episode.','',
        '| Capture | Candidate | Direction | Policy | Delay ms | Gross positive/known/surviving | Net positive/known/surviving | Net survival |','|---|---|---:|---|---:|---|---|---|']
    for r in combined['latency_sensitivity.csv']:
        if r['policy']=='balanced_asof':text.append(f"| {r['capture_id']} | {r['candidate']} | {r['direction']} | balanced | {r['latency_ms']} | {r.get('gross_positive_distinct_observations','NA')}/{r.get('gross_known','NA')}/{r.get('gross_positive_surviving','NA')} | {r['positive_distinct_observations']}/{r['known']}/{r['positive_surviving']} | {r['survival_known_only'] or 'null'} |")
    text+=['',f"Balanced episodes by quantity: gross={summary['gross_episodes']}, fee-positive={summary['fee_positive_episodes']}, budget-positive={summary['budget_positive_episodes']}. Duration, start/end, censoring and basis are in `opportunity_episodes.csv`. Quantity variants overlap; do not sum them as independent liquidity. Existing full/partial/failed/adverse hedge models are still exercised by synthetic approved tests; no real approved pair is present, so real strict stress PnL and residual contractual floor are unevaluated.",'',
        '## E. Maker feasibility',f"MAKER_QUOTE_FEASIBILITY_ONLY: {len(maker)} conditional size/state/venue rows, {summary['conditional_post_only_rows']} valid conditional post-only quotes, {summary['conditional_at_best_bid_rows']} at or above the local best bid. A low resting bid can be theoretically affordable while having no demonstrated fill probability. Tick rounding, local bid/ask, opposing hedge fees/depth and worst unhedged acquisition cost are recorded. The safety-plus-minimum-profit allowance is .01/share. Maker zero fees are assumed only where frozen series type / takerOnly metadata supports the official schedule. Proven payout floor, fills, queue position, rewards, strategy capacity and PnL remain null. Measured quote persistence is in `maker_quote_episodes.csv`; per-state duration, ceiling changes and 1000ms opposing hedge-cost drift are in the maker table. No orders were placed.",'',
        'Maker budget sensitivity (conditional price feasibility only): '+json.dumps(sensitivity,ensure_ascii=False),'At zero margin this is fee-only headroom, not positive risk-adjusted profit. Quote-row counts are overlapping size/state/venue observations, not orders or fill opportunities. All rows retain an unproved payout floor and null fills/PnL.','',
        '## F. $200K assessment','Two wallets remain independent at $100K each. `capital_feasibility.csv` records conditional acquisition/fee requirements for tested quantities, observed depth and conditional wallet utilization. It does not reserve/reuse the same cash or liquidity, infer order fills, or project a contractual return. Unknown settlement equivalence makes capital-day PnL and executable capacity null. Multiple nomination candidates share party/convention/collateral concentration and have multi-year holding proxies; Aliens has a shorter roughly 56-day contractual horizon, still without observed settlement receipts. Positive gross persistence does not turn a fee-negative trade into a capital allocation.','',
        '| Annual target | Required full-$200K turnovers at 1% net edge |','|---:|---:|']
    for target in [10000,25000,50000,100000]:text.append(f'| ${target:,} | {D(target)/D(2000)} |')
    text+=['','These are arithmetic requirements, not forecasts. The CSV also includes .1% and .5% edge cases; `annual_target_edges.json` gives required average edges at 1/4/12 full turns. Holding times, independent wallet constraints and available market depth limit turnover; no ten- or twenty-minute recording is annualized.','',
        '## G. Tests, clocks, limitations and audit',json.dumps(tests,ensure_ascii=False),'`health_audit.json` provides exact snapshot/delta counts, local sequence checks, disconnect/reopen durations, transport symptoms and reconciliation outcomes for each capture. Transport health is distinct from executable-ask availability and strict approved eligibility.',
        'Historical raw inputs remain immutable and are hashed before replay. Independent audit checks raw provenance, decision cutoffs, interval sums, episode overlap, exact fee/depth arithmetic and independently reconstructs Aliens WS depth for price verification. Repeated native analytical files are compared byte for byte. Root audit manifest binds resulting files and implementation hashes.',
        'The first nine-pair experiment was stopped after 205.061s because valid empty/omitted book sides triggered unnecessary whole-session reconnects. The partial capture and its explicit interrupted status remain separate evidence. The fix represents empty depth without inventing quotes and keeps unavailable-ask candidates out of economic eligibility. The subsequent 1200-second run is reported separately, including actual disconnects/gaps rather than inferred continuity.',
        'Clock diagnostics preserve exchange/local wall fields and monotonic sequencing. New raw captures include a hashed completion sidecar; replay availability is delayed until actual completion. Historical processing completion is unrecorded; historical availability is the original serialized receive mark, not a claim of zero CPU or precise one-way latency. Negative source-to-local differences are clock uncertainty. Current OS sync/offset/root-distance and public REST RTT samples are recorded without changing clocks or applying a fixed historical correction. CLOB public streams lack a contiguous sequence, so absence of local recording gaps does not prove zero exchange message loss.','',
        '## H. Recommended next action',f'**{recommendation}**. The measured diagnostics and fee/depth constraints above determine this recommendation; there is no approved real contractual arbitrage backtest. Extend the relationship universe toward short-duration, two-sided markets with complete payoff/asset/quantity evidence before allocating $200K. Do not promote conditional parity gaps or unfilled maker bids into profitability claims.','']
    (out/'report.md').write_text('\n'.join(text))
    write(out/'implementation_changes.json',CHANGES)
    files=[]
    for p in sorted(out.rglob('*')):
        if p.is_file() and p.name!='audit_manifest.json':files.append({'file':str(p.relative_to(out)),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
    code=[{'file':p,'sha256':hashlib.sha256((ROOT/p).read_bytes()).hexdigest()} for p in CHANGES if (ROOT/p).is_file()]
    write(out/'audit_manifest.json',{'schema':'cross-venue-v012-audit-manifest','files':files,'implementation':code,'strict_simulated_pnl':None,'verified_live_realized_pnl':None,'live_orders_sent':0})
    return {'recommendation':recommendation,'price_rows':len(prices),'net_positive':positive,'report':str(out/'report.md')}
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--analysis',nargs=2,action='append',required=True,metavar=('LABEL','DIR'));p.add_argument('--test-log',type=Path);a=p.parse_args();print(json.dumps(run(a.out,[(n,Path(d)) for n,d in a.analysis],a.test_log)))

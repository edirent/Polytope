"""Read-only official-source refresh and explicit pending review for nine candidates.
No state reconstruction, pricing, approval, execution or PnL is implemented here.
"""
import argparse,concurrent.futures,hashlib,json,platform,subprocess,time,urllib.request,urllib.error
from datetime import datetime,timezone
from pathlib import Path

def write(p,v):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(v,indent=2,ensure_ascii=False)+'\n')
def sha(b):return hashlib.sha256(b).hexdigest()
def retrieve(url):
    start=time.time_ns();mono=time.monotonic_ns();body=b'';status=0;error=None
    try:
        with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Polytope-readonly-review/0.1.2'}),timeout=12) as r:status=r.status;body=r.read(16*1024*1024)
    except urllib.error.HTTPError as e:status=e.code;body=e.read();error='HTTP_ERROR'
    except (OSError,TimeoutError):error='NETWORK_UNAVAILABLE'
    return {'url':url,'started_wall_ns':start,'received_wall_ns':time.time_ns(),'rtt_monotonic_ms':(time.monotonic_ns()-mono)/1e6,'http_status':status,'error':error,'sha256':sha(body)},body

def run(out):
    old=Path('runs/cross_venue/v0_1_1_review_final_20261105');nine=json.loads((old/'top20_dossiers.json').read_text())[:9];terms=old/'supplementary_terms'
    terms_manifest=json.loads((terms/'pdf_extraction_manifest.json').read_text());terms_evidence=[]
    for t in terms_manifest:
        text=terms/t['file'];assert sha(text.read_bytes())==t['text_sha256']
        source=terms/t['file'].removesuffix('.txt');assert sha(source.read_bytes())==t['source_sha256'];terms_evidence.append({**t,'source_file':str(source),'text_file':str(text)})
    urls=[]
    for d in nine:
        urls += ['https://external-api.kalshi.com/trade-api/v2/markets/'+d['kalshi_market'],'https://gamma-api.polymarket.com/markets/'+d['polymarket']['latest_raw_metadata']['id']]
    urls+=['https://docs.polymarket.com/trading/fees','https://kalshi.com/docs/kalshi-fee-schedule.pdf']
    sources=out/'pair_review_dossiers'/'refreshed_sources';sources.mkdir(parents=True,exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:results=list(pool.map(retrieve,urls))
    source_rows=[];current={}
    for i,(info,body) in enumerate(results):
        info['file']=f'{i:03}.body';(sources/info['file']).write_bytes(body);source_rows.append(info)
        try:current[info['url']]=json.loads(body) if info['http_status']==200 else None
        except (ValueError,UnicodeError):current[info['url']]=None
    write(sources/'sources.json',source_rows)
    notes=json.loads((terms/'rule_comparison_notes.json').read_text());by_note={x['candidate_pair_id']:x for x in notes}
    reviews=[];ranking=[]
    for i,d in enumerate(nine):
        kid=d['kalshi_market'];pid=d['polymarket_market'];alien=i==0
        ku='https://external-api.kalshi.com/trade-api/v2/markets/'+kid;pu='https://gamma-api.polymarket.com/markets/'+d['polymarket']['latest_raw_metadata']['id']
        kr=(current.get(ku) or {}).get('market');pr=current.get(pu)
        rules_changed=[]
        for venue,raw in [('kalshi',kr),('polymarket',pr)]:
            if raw:
                rules=raw.get('rules_primary','')+'\n'+raw.get('rules_secondary','') if venue=='kalshi' else raw.get('description','')
                if rules.strip()!=d[venue]['full_rules'].strip():rules_changed.append(venue)
        counterexamples=([
            {'state':'Definitive statement only by an additionally enumerated Kalshi Cabinet office','payoff_K_yes':'potentially 1','payoff_P_yes':'unresolved; not necessarily 1','why':'Cabinet definitions not certified equal'},
            {'state':'Credible reporting consensus without an admissible official-capacity statement','payoff_K_yes':'potentially 0','payoff_P_yes':'potentially 1','why':'Source/admissibility mismatch'},
            {'state':'Announcement at cutoff boundary or before issuance','payoff_K_yes':'depends on exact before-date and issuance clause','payoff_P_yes':'depends on 11:59 PM ET deadline','why':'Observation interval equality unproved'}] if alien else [
            {'state':'Nominee accepts, subsequently dies or is replaced','payoff_K_yes':'first accepting nominee retained under archived PRESNOM terms','payoff_P_yes':'original nominee retained under explicit replacement clause','why':'Broad agreement; need exception and dispute certification'},
            {'state':'Nomination contested, party convention canceled or no acceptance before deadline','payoff_K_yes':'No or outcome-review contingency unverified','payoff_P_yes':'No/NegRisk Other/oracle contingency unverified','why':'Complete all-state coverage not proved'}])
        # Every pair retains general payoff counterexamples until venue exception policies are established.
        counterexamples += [{'state':'One venue voids, refunds or pays a fractional amount while the other resolves Yes/No','payoff_K_yes':'unverified exception payout','payoff_P_yes':'unverified exception payout','why':'A nominal unit parity cannot be called a contractual floor'}, {'state':'Collateral depegs, freezes or is redeemed with conversion costs','payoff_K_yes':'USD contractual claim','payoff_P_yes':'actual collateral contract not established by market metadata','why':'Numerical prices are not asset equivalence'}]
        dossier={**d,'version':'0.1.2','prior_dossier_sha256':d['dossier_sha256'],'original_archived_dossier':str(old/'top20_dossiers.json'),
            'classification':'LOGICALLY_RELATED' if alien else 'UNVERIFIABLE','classification_scope':'Headline and restricted normal-state predicates only; no unconditional implication certified' if alien else 'Normal nomination-and-acceptance predicate broadly aligned; full exception/NegRisk/collateral states unverifiable',
            'comparison':by_note[d['candidate_pair_id']], 'full_contract_source_evidence':terms_evidence,
            'current_market_metadata':{'kalshi':kr,'polymarket':pr},'current_source_urls':[ku,pu],'rules_changed_since_archived_review':rules_changed,
            'oracle_relation_status':'DRAFT_HYPOTHESIS_ONLY; see native oracle_relationship_drafts.json; no excluded payoff states',
            'payoff_counterexamples':counterexamples,'payoff_coverage_complete':False,
            'cancellation_void_tie_dispute':{'kalshi':d['kalshi']['general_void_dispute_policy'],'polymarket':d['polymarket']['general_void_dispute_policy'],'status':'UNVERIFIED_ALL_STATE_COVERAGE','binary_tie':'No ordinary YES/NO tie; disputed/void outcomes still require proof'},
            'settlement_asset_status':'USD_VS_UNVERIFIED_POLY_COLLATERAL; reward asset address is not proof of market collateral',
            'quantity_status':'MINIMUM_PARTIALLY_KNOWN_STEP_UNVERIFIED; diagnostic probes cannot be orders',
            'fee_status':'Frozen market-specific taker schedules used only during historical validity; maker zero only where explicitly supported by frozen series type/takerOnly and official schedule',
            'book_scope':'Historical real synchronized WS only for Aliens; archived public REST for remaining candidates is NOT synchronized L2',
            'estimated_holding_days_proxy':d['score_components'].get('contractual_horizon_days_proxy'),
            'reviewer':None,'reviewed_at':None,'human_decision':None,'approval_status':'PENDING_HUMAN_REVIEW','coverage_proven':False}
        dossier.pop('dossier_sha256',None);dossier['dossier_sha256']=sha(json.dumps(dossier,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode())
        name=f'{i+1:02}_{kid}.json';write(out/'pair_review_dossiers'/name,dossier)
        reviews.append({'candidate_pair_id':d['candidate_pair_id'],'status':'PENDING_HUMAN_REVIEW','decision':None,'reviewer':None,'reviewed_at':None,'dossier_sha256':dossier['dossier_sha256'],'proposed_revision':d['proposed_revision'],'approved_revision':None,'approved_artifact_hash':None,'coverage_proven':False,'settlement_assets_reviewed':False,'quantity_constraints_reviewed':False,'dossier_file':'pair_review_dossiers/'+name})
        ranking.append({'rank':i+1,'kalshi_market':kid,'polymarket_market':pid,'classification':dossier['classification'],'economic_priority':'LOW_AFTER_NEGATIVE_TAKER_FEES; highest evidence readiness' if alien else 'NEEDS_SYNCHRONIZED_DEPTH; long capital lockup','holding_days_proxy':dossier['estimated_holding_days_proxy'],'strict_eligible':False,'rule_changed':rules_changed,'source_refresh_ok':bool(kr and pr)})
    write(out/'approval_manifest.pending.json',{'schema':'cross-venue-human-review-manifest-v0.1.1','reviews':reviews,'automatic_approval':False})
    write(out/'pair_review_dossiers'/'ranking.json',ranking)
    os_clock={}
    for name,command in [('timedatectl',['timedatectl','show','-p','NTPSynchronized','-p','TimeUSec','-p','Timezone']),('timesync_status',['timedatectl','timesync-status']),('ntpq',['ntpq','-pn']),('chronyc',['chronyc','tracking'])]:
        try:r=subprocess.run(command,capture_output=True,text=True,timeout=5);os_clock[name]={'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
        except (OSError,subprocess.TimeoutExpired):os_clock[name]={'status':'UNAVAILABLE'}
    import re
    status=os_clock.get('timesync_status',{}).get('stdout','')
    def number(field):
        m=re.search(field+r':\s*([+-]?[0-9.]+)ms',status);return float(m.group(1)) if m else None
    clocks={'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'platform':platform.platform(),'system_status':os_clock,'offset_ms':number('Offset'),'uncertainty_ms':number('Root distance'),'offset_uncertainty_caveat':'Root distance is the daemon bound estimate, not proof of exchange clock alignment',
        'historical_ntp_observation':{'source':'runs/cross_venue/v0_1_1_authenticated_20261105_3/validation/ntp_status.txt','offset_ms':144.494,'delay_ms':105.029,'jitter_ms':34.852,'not_a_per_packet_correction':True},
        'rest_rtt_samples':[{'url':i['url'],'http_status':i['http_status'],'rtt_ms':i['rtt_monotonic_ms']} for i in source_rows],
        'one_way_latency':None,'fixed_offset_subtracted':False,'system_clock_changed':False,'processing_completion_historical':'NOT_RECORDED; callback-to-availability is recorder delay only',
        'source_clock_interpretation':'Negative source-to-local ages are clock differences, not negative network latency'}
    rtts=sorted(i['rtt_monotonic_ms'] for i in source_rows);clocks['rest_rtt_distribution_ms']={'n':len(rtts),'p50':rtts[len(rtts)//2],'p95':rtts[int((len(rtts)-1)*.95)],'max':rtts[-1]}
    write(out/'clock_diagnostics.json',clocks)
    return {'reviewed':len(reviews),'approved':0,'refresh_ok':sum(bool(x['source_refresh_ok']) for x in ranking)}
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();print(json.dumps(run(a.out)))

"""Evidence-backed triage of an immutable candidate queue; never grants approval.

Python is a read-only research tool. Native V0.1 remains the data/state/execution
path. Scores are triage heuristics, not a proof of payoff equivalence.
"""
import argparse
import csv
import hashlib
import json
import math
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def date(value):
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (ValueError, AttributeError):
        return None


def family(m):
    text = (m['title'] + ' ' + m['rules']).lower()
    if 'top-ranked llm' in text or 'best ai model' in text:
        return 'ai_leaderboard'
    if 'extraterrestrial' in text:
        return 'aliens'
    if 'nomination' in text or 'nominee for the presidency' in text:
        return 'presidential_nomination'
    if 'bitcoin' in text:
        return 'bitcoin'
    if 'esoccer' in text:
        return 'esoccer_single_match'
    if any(s in text for s in ['nba', 'goals', 'saint-germain', 'san antonio', 'los angeles']):
        return 'sports_season' if any(s in text for s in ['championship', 'nba finals']) else 'sports_single_match'
    return 'other:' + re.sub(r'[^a-z]', '', m['title'].lower())[:35]


def ai_company(m):
    text = m['rules'].split('\n')[0] if m['venue'] == 'kalshi' else m['title']
    pattern = r'If (.*?) has the top-ranked' if m['venue'] == 'kalshi' else r'Will (.*?) have the best AI'
    match = re.search(pattern, text, re.I)
    return match.group(1).lower().strip() if match else None


def compare(k, p):
    """Fail closed on concrete contradictions; retain exact evidence in output."""
    flags = []
    fk, fp = family(k), family(p)
    compatibility = 0.45
    if fk != fp:
        flags.append('DIFFERENT_EVENT_PREDICATE')
        compatibility = 0
    elif fk == 'ai_leaderboard':
        if ai_company(k) != ai_company(p):
            flags.append('DIFFERENT_AI_COMPANY')
            compatibility = 0
        # These are different deterministic tie rules, not merely missing text.
        if 'most votes' in k['rules'].lower() and 'alphabetical' in p['rules'].lower():
            flags.append('CONFLICTING_TIEBREAK_PAYOFF')
            compatibility = 0
        if ('november' in k['title'].lower()) != ('november' in p['title'].lower()):
            flags.append('DIFFERENT_OBSERVATION_MONTH')
            compatibility = 0
        flags.append('EXACT_OBSERVATION_TIME_AND_AUTOEVAL_REVIEW_REQUIRED')
    elif fk == 'aliens':
        # The specific deadline must be compared, not the shared year token.
        if k['market_id'] != 'KXALIENS-27':
            flags.append('DIFFERENT_CONFIRMATION_DEADLINE')
            compatibility = 0
        else:
            flags += ['CABINET_DEFINITION_REVIEW_REQUIRED', 'EXACT_CUTOFF_TIME_REVIEW_REQUIRED',
                      'CREDIBLE_REPORTING_AND_OFFICIAL_SOURCE_REVIEW_REQUIRED']
            compatibility = 0.8
    elif fk == 'bitcoin':
        kt, pt = k['rules'].lower(), p['rules'].lower()
        if 'binance' in pt and 'binance' not in kt:
            flags.append('DIFFERENT_PRICE_ORACLE')
            compatibility = 0
        flags += ['EXACT_STRIKE_AND_INEQUALITY_REVIEW_REQUIRED', 'EXACT_PRICE_OBSERVATION_TIME_REVIEW_REQUIRED']
        if any(s in p['title'].lower() for s in ['satoshi', 'dip to', 'reach', 'hit']):
            flags.append('DIFFERENT_EVENT_PREDICATE')
            compatibility = 0
    elif fk == 'presidential_nomination':
        flags += ['WIN_VERSUS_ACCEPT_NOMINATION_REVIEW_REQUIRED', 'WITHDRAWAL_DEATH_SUBSTITUTION_REVIEW_REQUIRED']
        compatibility = 0.75
    if (k['rules'].lower().find('$0.33') >= 0) != (p['rules'].lower().find('$0.33') >= 0):
        flags.append('FRACTIONAL_VOID_PAYOFF_NOT_COVERED')
        compatibility = 0
    if p['raw'].get('negRisk'):
        flags.append('NEGRISK_STRUCTURE_REVIEW_REQUIRED')
    return fk, fp, compatibility, flags


def rank_candidates(candidates, markets, as_of):
    by = {(m['venue'], m['market_id']): m for m in markets}
    ranked = []
    for original_rank, candidate in enumerate(candidates, 1):
        k = by['kalshi', candidate['kalshi_market']]
        p = by['polymarket_international', candidate['polymarket_market']]
        fk, fp, compatible, flags = compare(k, p)
        horizons = [date(m['raw'].get('expected_expiration_time', m['close_time'])) for m in (k, p)]
        days = max([(d-as_of).total_seconds()/86400 for d in horizons if d] or [3650])
        liquid = math.log1p(max(0, min(k['volume_24h'], p['volume_24h']))) / 15
        fee_known = sum(bool(m.get('fee_metadata')) for m in (k, p))
        data_ready = sum(m['tradable'] and m['protocol_supported'] for m in (k, p))
        size_known = sum(Decimal(m['quantity_step']) > 0 and Decimal(m['minimum_quantity']) > 0 for m in (k,p))
        if size_known < 2:
            flags.append('QUANTITY_CONSTRAINTS_UNVERIFIED')
        if any(m['yes']['currency'].startswith('UNKNOWN') for m in (k,p)):
            flags.append('SETTLEMENT_ASSET_UNVERIFIED')
        flags += ['GENERAL_VOID_AND_DISPUTE_POLICIES_UNVERIFIED', 'HUMAN_APPROVAL_REQUIRED', 'KALSHI_WS_AUTH_UNVERIFIED']
        # Compatibility dominates; short horizon and liquid quotes only break
        # ties within the same semantic tier. No threshold approves a pair.
        score = compatible*1000 + (80 if fk == fp else 0) + candidate['match_score']*30 + min(1, liquid)*20 + 20/(1+max(0,days)) + data_ready*2 + fee_known + size_known
        ranked.append({**candidate, 'original_rank':original_rank, 'research_score':round(score,6),
                       'event_family_kalshi':fk, 'event_family_polymarket':fp,
                       'rule_compatibility_heuristic':compatible,
                       'review_disposition':'NEEDS_HUMAN_REVIEW' if compatible>0 else 'CONTRADICTORY_OR_UNRELATED',
                       'score_components':{'compatibility':compatible, 'similarity':candidate['match_score'],
                         'minimum_venue_volume_24h':min(k['volume_24h'],p['volume_24h']),
                         'minimum_liquidity_proxy':min(k['liquidity'],p['liquidity']),
                         'fee_metadata_present_venues':fee_known,'tradable_supported_venues':data_ready,
                         'contractual_horizon_days_proxy':round(days,6), 'expected_cash_settlement_time':'UNKNOWN'},
                       'blockers':sorted(set(flags)), 'approval_status':'PENDING_HUMAN_REVIEW'})
    ranked.sort(key=lambda r:(-r['research_score'], -r['match_score'],r['candidate_pair_id']))
    for i,row in enumerate(ranked,1):row['review_rank']=i
    return ranked


class EvidenceFetcher:
    def __init__(self, root):
        self.root, self.records, self.cache = root, [], {}
        root.mkdir()

    def get(self, url):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme != 'https' or parsed.hostname not in {
            'external-api.kalshi.com','gamma-api.polymarket.com','clob.polymarket.com'}:
            raise ValueError('source must be an official public HTTPS API')
        if url in self.cache:
            return self.cache[url]
        body,status,error = b'',0,''
        for attempt in range(3):
            started = time.time_ns()
            try:
                request=urllib.request.Request(url,headers={'User-Agent':'Polytope-read-only-research/0.1.1'})
                with urllib.request.urlopen(request,timeout=15) as response:
                    status,body=response.status,response.read(16*1024*1024)
            except urllib.error.HTTPError as exc:
                status,body,error=exc.code,exc.read(),str(exc)
            except (OSError,TimeoutError) as exc:
                error=str(exc)
            name=f'{len(self.records):04d}.body'
            (self.root/name).write_bytes(body)
            self.records.append({'url':url,'status':status,'error':error,'attempt':attempt,
                'started_wall_ns':started,'received_wall_ns':time.time_ns(),'file':name,'sha256':digest(body)})
            if status==200:
                try:self.cache[url]=json.loads(body)
                except (ValueError,UnicodeDecodeError):self.cache[url]=None
                break
            self.cache[url]=None
            if status not in (0,408,429) and status<500:break
            time.sleep(1+attempt*2)
        time.sleep(.15)
        return self.cache[url]


def market_detail(m, fetch=None):
    raw=m['raw'];sources=[]
    if m['venue']=='kalshi':
        url='https://external-api.kalshi.com/trade-api/v2/markets/'+urllib.parse.quote(m['market_id'])
        event_url='https://external-api.kalshi.com/trade-api/v2/events/'+urllib.parse.quote(m['event_id'])
        data=fetch.get(url) if fetch else None
        if data and isinstance(data.get('market'),dict):raw=data['market']
        event=fetch.get(event_url) if fetch else None
        series_id=(event or {}).get('event',{}).get('series_ticker')
        series_url='https://external-api.kalshi.com/trade-api/v2/series/'+series_id if series_id else None
        series=fetch.get(series_url) if fetch and series_url else None
        sources=[url,event_url]+([series_url] if series_url else [])
        rules=raw.get('rules_primary','')+'\n'+raw.get('rules_secondary','')
        fees={'market_override':(event or {}).get('event',{}), 'series':(series or {}).get('series',{}),
              'model_source':'https://kalshi.com/docs/kalshi-fee-schedule.pdf',
              'frozen_model':json.loads(Path('config/cross_venue_fee_models.json').read_text())['kalshi']}
        book_url=url+'/orderbook?depth=64';book=fetch.get(book_url) if fetch else None
    else:
        url='https://gamma-api.polymarket.com/markets/'+str(raw['id'])
        data=fetch.get(url) if fetch else None
        if isinstance(data,dict) and data.get('conditionId')==m['market_id']:raw=data
        sources=[url]
        rules=raw.get('description','')
        fees={k:raw.get(k) for k in ['feesEnabled','feeType','feeSchedule','makerBaseFee','takerBaseFee']}
        book_url='https://clob.polymarket.com/book?token_id='+m['yes']['instrument_id']
        book=fetch.get(book_url) if fetch else None
    sources.append(book_url)
    clauses=[s for s in re.split(r'(?<=\.)\s+|\n+',rules) if any(w in s.lower() for w in
             ['cancel','void','postpon','tie','unavailable','withdraw','death','otherwise','other','consensus','reporting'])]
    return {'venue':m['venue'],'market_id':m['market_id'],'title':raw.get('title',raw.get('question',m['title'])),
            'full_rules':rules,'rules_sha256':digest(rules.encode()),'prior_rules_sha256':m['rules_hash'],
            'rules_changed':digest(rules.encode())!=m['rules_hash'],'sources':sources,
            'cancellation_void_tie_source_clauses':clauses,
            'general_void_dispute_policy':'UNVERIFIED; absence of a clause is not proof that cancellation is impossible',
            'currency':m['yes']['currency'],'collateral_api_fields':{k:raw.get(k) for k in ['collateralToken','collateralAddress','version','negRisk']},
            'quantity_constraints':{k:raw.get(k) for k in ['minimum_order_size_fp','quantity_step_fp','orderMinSize','quantityStep','orderPriceMinTickSize','price_ranges']},
            'fee_evidence':fees,'latest_raw_metadata':raw,'latest_public_book':book,
            'book_evidence':'REST_SNAPSHOT_DIAGNOSTIC_ONLY' if book else 'UNAVAILABLE',
            'resolution_source':raw.get('resolutionSource') or 'See exact rule text and series settlement_sources',
            'cash_settlement_time':'UNKNOWN; endDate/close_time is not a receipt of settlement'}


def payoff_review(row,k,p):
    states=[{'state':'both_venues_agree_yes','kalshi_yes':'1','polymarket_no':'0','total':'1'},
            {'state':'both_venues_agree_no','kalshi_yes':'0','polymarket_no':'1','total':'1'},
            {'state':'kalshi_no_polymarket_yes','kalshi_yes':'0','polymarket_no':'0','total':'0'},
            {'state':'kalshi_yes_polymarket_no','kalshi_yes':'1','polymarket_no':'1','total':'2'},
            {'state':'void_or_fractional_or_disputed','kalshi_yes':None,'polymarket_no':None,'total':None}]
    return {'direction':'BUY_KALSHI_YES_AND_POLYMARKET_NO', 'coverage_proven':False,
            'common_currency_proven':False,'states':states,
            'counterexample_or_review_requirements':row['blockers'],
            'reverse_direction_requires_same_rule_proof':True,
            'warning':'Rows with disagreement cannot be deleted without a rule proof. USD and unknown collateral are not additive risk-free cash.'}


def build(run,out,refresh=False):
    if out.exists() and any(out.iterdir()):raise ValueError('review output must be new/empty')
    out.mkdir(parents=True,exist_ok=True)
    candidates=json.loads((run/'candidate_pairs.json').read_text())
    markets=json.loads((run/'market_metadata.json').read_text())['markets']
    capture=json.loads((run/'capture_manifest.json').read_text())
    as_of=date(capture['end_utc'])
    ranked=rank_candidates(candidates,markets,as_of)
    by={(m['venue'],m['market_id']):m for m in markets}
    write(out/'ranked_candidates.json',ranked)
    with (out/'ranked_candidates.csv').open('w',newline='') as stream:
        columns=['review_rank','original_rank','candidate_pair_id','kalshi_market','polymarket_market',
                 'kalshi_title','polymarket_title','review_disposition','research_score','blockers','score_components']
        writer=csv.DictWriter(stream,columns,extrasaction='ignore');writer.writeheader()
        for row in ranked:writer.writerow({k:json.dumps(v) if isinstance(v,(list,dict)) else v for k,v in row.items() if k in columns})
    fetch=EvidenceFetcher(out/'source_responses') if refresh else None
    details={};dossiers=[]
    dossier_dir=out/'dossiers';dossier_dir.mkdir()
    for row in ranked[:20]:
        pair=[]
        for venue,key in [('kalshi','kalshi_market'),('polymarket_international','polymarket_market')]:
            identity=(venue,row[key])
            if identity not in details:details[identity]=market_detail(by[identity],fetch)
            pair.append(details[identity])
        k,p=pair;dossier={**row,'kalshi':k,'polymarket':p,'payoff_review':payoff_review(row,k,p),
            'proposed_revision':1,'reviewer':None,'reviewed_at':None,'human_decision':None}
        dossier['dossier_sha256']=digest(json.dumps(dossier,sort_keys=True,separators=(',',':')).encode())
        dossiers.append(dossier)
        stem=f"{row['review_rank']:02d}_{row['candidate_pair_id'][:12]}"
        write(dossier_dir/(stem+'.json'),dossier)
        md=[f"# Candidate {row['review_rank']}: {k['title']} / {p['title']}",
            f"Status: **PENDING HUMAN REVIEW**. Disposition: {row['review_disposition']}.",
            f"ID: `{row['candidate_pair_id']}`; proposed revision 1; dossier hash `{dossier['dossier_sha256']}`.",
            '## Material differences / blockers','\n'.join('- '+b for b in row['blockers']),
            '## Holding period and liquidity',json.dumps(row['score_components'],indent=2)]
        for label,side in [('Kalshi',k),('Polymarket International',p)]:
            md += [f'## {label}: exact settlement text',side['full_rules'],
                   'Sources: '+', '.join(f'[{u}]({u})' for u in side['sources']),
                   f"Rules hash: `{side['rules_sha256']}`; changed since capture: {side['rules_changed']}.",
                   'Cancellation, void, tie and source clauses: '+json.dumps(side['cancellation_void_tie_source_clauses'],ensure_ascii=False),
                   side['general_void_dispute_policy'],
                   'Currency: '+side['currency']+'; quantities/tick: '+json.dumps(side['quantity_constraints']),
                   'Fees: '+json.dumps(side['fee_evidence'],ensure_ascii=False),
                   'Public-book availability: '+side['book_evidence']+'; '+side['cash_settlement_time']]
        md += ['## Payoff coverage',json.dumps(dossier['payoff_review'],indent=2),
               'Human must resolve every material scenario, pin assets/constraints/fees, and explicitly approve a revision. This dossier does not enable capture trading or execution.']
        (dossier_dir/(stem+'.md')).write_text('\n\n'.join(md)+'\n')
    write(out/'top20_dossiers.json',dossiers)
    manifest={'schema':'cross-venue-human-review-manifest-v0.1.1','source_run':str(run),
        'source_candidate_sha256':digest((run/'candidate_pairs.json').read_bytes()),
        'source_metadata_sha256':digest((run/'market_metadata.json').read_bytes()),
        'pairs':[], 'reviews':[{'candidate_pair_id':d['candidate_pair_id'],'proposed_revision':1,
            'dossier_sha256':d['dossier_sha256'],'status':'PENDING_HUMAN_REVIEW','reviewer':None,
            'decision':None,'blockers':d['blockers'],'coverage_proven':False} for d in dossiers],
        'ingestion_policy':'Never load this manifest directly. Only separately explicit APPROVED revisions with resolved payoff/currency/quantity evidence may enter the native approval loader.',
        'verified_live_realized_pnl':None}
    write(out/'approval_manifest.pending.json',manifest)
    if fetch:write(out/'source_requests.json',fetch.records)
    summary={'schema':'cross-venue-review-results-v0.1.1','candidates_inspected':len(ranked),
        'top_dossiers':len(dossiers),'potentially_related_needs_human_review':sum(r['rule_compatibility_heuristic']>0 for r in ranked),
        'contradictory_or_unrelated':sum(r['rule_compatibility_heuristic']==0 for r in ranked),
        'human_reviewed_pairs':0,'approved_pairs':0,'review_stage':'AUTOMATED_RULE_TRIAGE_WITH_EXACT_SOURCE_DOSSIERS',
        'blocker_counts':dict(Counter(b for r in ranked for b in r['blockers'])),
        'source_http_statuses':dict(Counter(str(r['status']) for r in fetch.records)) if fetch else {},
        'ranking_as_of':as_of.isoformat(),'ranking_metric_note':'Holding period is a contractual horizon proxy; liquidity is metadata, not executable capacity.',
        'verified_live_realized_pnl':None}
    write(out/'summary.json',summary)
    lines=['# V0.1.1 ranked human-review dossier',json.dumps(summary,indent=2),
        '|Rank|Kalshi|Polymarket|Disposition|Dossier|','|---:|---|---|---|---|']
    for row in ranked[:20]:
        stem=f"{row['review_rank']:02d}_{row['candidate_pair_id'][:12]}"
        lines.append(f"|{row['review_rank']}|{row['kalshi_title']}|{row['polymarket_title']}|{row['review_disposition']}|[rules](dossiers/{stem}.md)|")
    lines += ['All 200 candidates are preserved in ranked_candidates.json/csv. No human approval or PnL is inferred.',
              'The queue is a bounded sample, not an exhaustive cross-venue search. Short-duration false matches do not outrank compatible predicates.']
    (out/'report.md').write_text('\n\n'.join(lines)+'\n')
    inventory=[{'file':str(p.relative_to(out)),'sha256':digest(p.read_bytes())} for p in sorted(out.rglob('*')) if p.is_file()]
    write(out/'audit_manifest.json',{'files':inventory,'research_script_sha256':digest(Path(__file__).read_bytes())})
    return summary


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir',required=True,type=Path)
    parser.add_argument('--out',required=True,type=Path)
    parser.add_argument('--refresh',action='store_true',help='Fetch official public market/rule/fee/book sources for top20; save every response')
    args=parser.parse_args()
    print(json.dumps(build(args.run_dir,args.out,args.refresh),indent=2))

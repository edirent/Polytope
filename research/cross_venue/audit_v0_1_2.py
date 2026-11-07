"""Independent evidence/arithmetic audit. No networking, approval or order access."""
import argparse,csv,hashlib,json
from collections import defaultdict
from decimal import Decimal as D,ROUND_CEILING
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def rows(p):return list(csv.DictReader(p.open()))
def micro(s):return int(D(str(s))*1000000)
def audit(source,out,repeat=None):
    m=json.loads((source/'capture_manifest.json').read_text());raw=[];byhash=defaultdict(list)
    for f in m['raw_inventory']:
        p=source/f['file'];assert sha(p)==f['sha256'];offset=0
        for line in p.read_bytes().splitlines(keepends=True):
            r=json.loads(line);assert r['raw_log_offset']==offset;offset+=len(line);assert hashlib.sha256(r['raw_payload'].encode()).hexdigest()==r['raw_hash'];raw.append(r);byhash[r['raw_hash']].append(r)
    raw.sort(key=lambda x:x['record_sequence'])
    if m.get('processing_timing_file'):
        timing=source/m['processing_timing_file'];assert sha(timing)==m['processing_timing_sha256'];timings=[json.loads(line) for line in timing.open()];assert len(timings)==len(raw)
        for r,t in zip(raw,timings):
            assert r['record_sequence']==t['record_sequence'] and r['raw_hash']==t['raw_hash'];assert t['processing_completed_monotonic_ns']>=r['availability_monotonic_ns'];r['availability_monotonic_ns']=t['processing_completed_monotonic_ns']
    assert [r['record_sequence'] for r in raw]==list(range(1,len(raw)+1))
    price=rows(out/'price_dislocations.csv');checks=0
    for r in price:
        q=micro(r['quantity']);k=micro(r['kalshi_cost']);p=micro(r['poly_cost']);kf=micro(r['kalshi_fee']);pf=micro(r['poly_fee']);gross=micro(r['gross_parity_discrepancy']);net=micro(r['conditional_net_discrepancy']);budget=micro(r['forward_and_conversion_budget'])
        assert gross==q-k-p and net==gross-kf-pf-budget and micro(r['fee_adjusted_discrepancy'])==gross-kf-pf
        assert r['mode']=='UNAPPROVED_PRICE_DIAGNOSTIC' and not r['strict_pnl'] and not r['contractual_payout_floor']
        t=int(r['decision_mono_ns'])
        for venue in ['kalshi','poly']:
            assert int(r[venue+'_recv_ns'])<=t and int(r[venue+'_authority_ns'])<=t and int(r[venue+'_last_change_ns'])<=t
            matches=[rr for rr in byhash[r[venue+'_raw_hash']] if rr['raw_log_offset']==int(r[venue+'_raw_offset']) and rr['recv_monotonic_ns']==int(r[venue+'_recv_ns'])];assert matches;assert any(rr['availability_monotonic_ns']<=t for rr in matches)
        checks+=1
    health=rows(out/'joint_book_health.csv');intervals=defaultdict(list)
    for r in health:
        a,b=int(r['decision_mono_ns']),int(r['end_mono_ns']);assert b>=a
        intervals[(r['candidate'],r['policy'],r['direction'])].append((a,b,r['reason']))
    policy_by_key={(c['candidate'],c['policy'],c['direction']):c for c in rows(out/'asof_coverage_comparison.csv')}
    for h in health:
        if h['eligible']!='true':continue
        p=policy_by_key[(h['candidate'],h['policy'],h['direction'])];last=int(h['end_mono_ns'])-1
        for venue in ['kalshi','poly']:
            authority=int(h[venue+'_authority_ns']);changed=int(h[venue+'_last_change_ns']);live=int(h[venue+'_liveness_ns'])
            assert last-authority<=int(p['authority_limit_ms'])*1000000
            assert last-live<=int(p['liveness_limit_ms'])*1000000
            assert last-changed<=int(p['change_limit_ms'])*1000000 or authority>=changed, 'eligibility crosses an unconfirmed change-age expiry'
    for c in rows(out/'asof_coverage_comparison.csv'):
        window=D(c['window_sec']);assert 0<=D(c['eligible_sec'])<=window
        assert abs(sum((D(v) for k,v in c.items() if k.startswith('exclusive_') and v),D(0))-window)<D('.00000001')
        x=intervals[(c['candidate'],c['policy'],c['direction'])];assert all(a[1]<=b[0] for a,b in zip(x,x[1:]));eligible=sum(b-a for a,b,r in x if r=='ELIGIBLE');assert abs(D(eligible)/10**9-D(c['eligible_sec']))<D('.00000001')
    for r in rows(out/'latency_observations.csv'):
        assert int(r['execution_ns'])-int(r['observed_ns'])==int(r['latency_ms'])*1000000
        if r['outcome']=='CONDITIONAL_COST_OBSERVATION':assert int(r['execution_k_recv_ns'])<=int(r['execution_ns']) and int(r['execution_p_recv_ns'])<=int(r['execution_ns']);assert D(r['original_net'])-D(r['later_net'])==D(r['deterioration'])
        else:assert not r['later_net']
    episode_rows=rows(out/'opportunity_episodes.csv');episodes=[r for r in episode_rows if r.get('start_mono_ns')];groups=defaultdict(list)
    for r in episode_rows:
        if not r.get('start_mono_ns'):assert r.get('status')=='UNAVAILABLE' and r.get('reason') and not r.get('strict_pnl')
    for r in episodes:
        a,b=int(r['start_mono_ns']),int(r['end_mono_ns']);assert b>=a;assert abs(D(b-a)/10**9-D(r['duration_sec']))<D('.00000001');assert not r['strict_pnl'];groups[(r['candidate'],r['policy'],r['direction'],r['quantity'],r['basis'])].append((a,b))
    for g in groups.values():assert all(a[1]<=b[0] for a,b in zip(sorted(g),sorted(g)[1:])), 'overlapping duplicate episodes'
    # Independent raw WS depth reconstruction for observed cost verification only.
    selected=json.loads((source/'selected_markets.json').read_text());kid=next(m for m in selected if m['market_id']=='KXALIENS-27');pid=next(m for m in selected if m['market_id'].startswith('0x747dc809'))
    tokens={pid['yes']['instrument_id']:'poly_yes_cost',pid['no']['instrument_id']:'poly_no_cost'};kb={'yes':{},'no':{}};pb={a:{'bids':{},'asks':{}} for a in tokens}
    grouped=defaultdict(list)
    for r in price:
        if r['candidate'].startswith('KXALIENS-27') or r['candidate']=='aliens_candidate_pending_review':grouped[int(r['decision_mono_ns'])].append(r)
    ri=0;walks=0
    def update(book,p,q):
        p,q=micro(p),micro(q)
        if q:book[p]=q
        else:book.pop(p,None)
    def consume(r):
        if r['transport']!='ws' or r['classification']!='market_data':return
        try:packet=json.loads(r['raw_payload'])
        except ValueError:return
        if r['venue']=='kalshi':
            body=packet.get('msg',{});kind=packet.get('type')
            if body.get('market_ticker')!='KXALIENS-27':return
            if kind=='orderbook_snapshot':
                for side in kb:
                    kb[side]={}
                    for p,q in body.get(side+'_dollars_fp',[]):update(kb[side],p,q)
            elif kind=='orderbook_delta':
                side=body['side'];p=micro(body['price_dollars']);q=kb[side].get(p,0)+micro(body['delta_fp']);assert q>=0
                if q:kb[side][p]=q
                else:kb[side].pop(p,None)
        else:
            for e in packet if isinstance(packet,list) else [packet]:
                kind=e.get('event_type')
                if kind=='book' and e.get('asset_id') in pb:
                    b=pb[e['asset_id']]
                    for side in ['bids','asks']:
                        b[side]={}
                        for x in e.get(side,[]):update(b[side],x['price'],x['size'])
                elif kind=='price_change':
                    for x in e.get('price_changes',[]):
                        if x.get('asset_id') in pb:update(pb[x['asset_id']]['bids' if x['side']=='BUY' else 'asks'],x['price'],x['size'])
    def walk(asks,q):
        remain=q;cost=0
        for p,n in sorted(asks.items())[:64]:take=min(remain,n);cost+=p*take;remain-=take
        return (cost+999999)//1000000 if remain==0 else None
    for t,observed in sorted(grouped.items()):
        while ri<len(raw) and raw[ri]['availability_monotonic_ns']<=t:consume(raw[ri]);ri+=1
        asks={'kalshi_yes_cost':{1000000-p:q for p,q in kb['no'].items()},'kalshi_no_cost':{1000000-p:q for p,q in kb['yes'].items()}}
        asks.update({tokens[a]:b['asks'] for a,b in pb.items()})
        for r in observed:
            for name,book in asks.items():
                if r.get(name+'_status') not in (None,'QUOTED','INSUFFICIENT_VISIBLE_DEPTH'):
                    assert not r[name];continue
                expected=walk(book,micro(r['quantity']));actual=micro(r[name]) if r[name] else None
                assert expected==actual,(name,t,expected,actual);walks+=1
    summary=json.loads((out/'economic_summary.json').read_text());assert summary['verified_live_realized_pnl'] is None and summary['strict_simulated_pnl'] is None and summary['live_orders_sent']==0 and summary['approved_pairs']==0
    deterministic=0
    if repeat:
        for p in out.iterdir():
            if p.is_file():assert p.read_bytes()==(repeat/p.name).read_bytes(),p.name;deterministic+=1
    return {'status':'INDEPENDENT_V012_AUDIT_PASSED','raw_records':len(raw),'price_rows_arithmetic_and_provenance_checked':checks,'independent_raw_L2_VWAP_checks':walks,'independent_depth_scope':'Aliens, four sides; all candidates receive arithmetic and provenance checks','coverage_rows_checked':len(rows(out/'asof_coverage_comparison.csv')),'health_intervals_checked':len(health),'episodes_checked':len(episodes),'deterministic_files_compared':deterministic,'strict_simulated_pnl':None,'live_orders_sent':0,'scope':'Independent raw price/depth walk, source cutoffs, arithmetic, intervals, episode dedup and byte equality; no proof of settlement or hypothetical fills'}
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--repeat',type=Path);a=p.parse_args();print(json.dumps(audit(a.source,a.out,a.repeat),indent=2))

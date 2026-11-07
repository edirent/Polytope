"""Bounded public-data preflight; selects real instruments belonging to native proofs.
No pricing/reconstruction, approval, private authentication or orders occur here.
"""
import argparse,hashlib,json,time,urllib.request,urllib.error
from collections import Counter
from datetime import datetime,timezone
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def write(p,v):p.write_text(json.dumps(v,indent=2,ensure_ascii=False)+'\n')
def prepare(proofs,out,config_path):
    out.mkdir(parents=True,exist_ok=False);cfg=json.loads((ROOT/'config/cross_venue_v0_1_3.json').read_text());relationships=[json.loads(x) for x in proofs.open()];cache={};requests=[];old=set()
    for capture in cfg['historical_captures']:
        for m in json.loads((ROOT/capture/'selected_markets.json').read_text()):old.add((m['venue'],m['market_id']))
    def fetch(url):
        if url in cache:return cache[url]
        if not url.startswith(('https://external-api.kalshi.com/trade-api/v2/markets/','https://gamma-api.polymarket.com/markets/','https://clob.polymarket.com/book?token_id=')):raise ValueError('nonofficial data URL')
        start=time.time_ns();raw=b'';status=0;error=None
        try:
            with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Polytope-readonly-v0.1.3'}),timeout=10) as r:status=r.status;raw=r.read(8*1024*1024)
        except urllib.error.HTTPError as e:status=e.code;raw=e.read();error='HTTP_ERROR'
        except (OSError,TimeoutError):error='NETWORK_UNAVAILABLE'
        name=f'{len(requests):04}.body';(out/name).write_bytes(raw);requests.append({'url':url,'http_status':status,'started_wall_ns':start,'received_wall_ns':time.time_ns(),'sha256':hashlib.sha256(raw).hexdigest(),'file':name,'error':error})
        try:cache[url]=json.loads(raw) if status==200 else None
        except ValueError:cache[url]=None
        return cache[url]
    def clock(s):
        try:return datetime.fromisoformat(s.replace('Z','+00:00')).timestamp()
        except (ValueError,AttributeError):return 0
    priority={'FOUR_LEG_LADDER':0,'THREE_LEG_LADDER':1,'NESTED_THRESHOLD':2,'MUTUALLY_EXCLUSIVE_NO_BUNDLE':3,'SAME_MARKET_COMPLEMENT':4}
    choices=[]
    for r in relationships:
        if r['type'] not in priority or r['hypothetical_normal_minimum']<=0:continue
        markets={(l['market']['venue'],l['market']['market_id']):l['market'] for l in r['legs']}
        if any(k in old for k in markets):continue
        if any(clock(m['close_time'])<time.time()+2400 for m in markets.values()):continue
        choices.append(r)
    category_priority={'crypto':0,'economics':1,'sports':2,'other':3,'elections':4}
    choices.sort(key=lambda r:(category_priority.get(r['category'],5),priority[r['type']],-min(max(l['market']['liquidity'],l['market']['volume_24h']) for l in r['legs']),r['relationship_id']))
    selected=[];markets={};counts=Counter();rejected=[];seen_types=Counter()
    for relationship in choices:
        if len(selected)>=12:break
        venue=relationship['legs'][0]['market']['venue'];category=relationship['category'];typ=relationship['type']
        if counts[(venue,category)]>=3 or seen_types[(venue,typ)]>=2:continue
        proposed={(l['market']['venue'],l['market']['market_id']):l['market'] for l in relationship['legs']}
        if sum(k[0]==venue for k in set(markets)|set(proposed))>12:continue
        good=True
        for leg in relationship['legs']:
            m=leg['market'];v=m['venue'];raw=m['raw']
            url='https://external-api.kalshi.com/trade-api/v2/markets/'+m['market_id'] if v=='kalshi' else 'https://gamma-api.polymarket.com/markets/'+raw['id']
            current=fetch(url);current=(current or {}).get('market') if v=='kalshi' else current
            if not current or clock(current.get('close_time',current.get('endDate')))<time.time()+2100:good=False;reason='MARKET_CHANGED_OR_TOO_CLOSE';break
            if v=='kalshi':
                book=fetch(url+'/orderbook?depth=64');side='no_dollars' if leg['outcome_side']=='YES_OR_UP' else 'yes_dollars';has_asks=bool((book or {}).get('orderbook_fp',{}).get(side))
            else:
                token=leg['instrument']['instrument_id'];book=fetch('https://clob.polymarket.com/book?token_id='+token);has_asks=bool((book or {}).get('asks'))
            if not has_asks:good=False;reason='NO_VISIBLE_REQUIRED_ASK_SIDE';break
        if not good:rejected.append({'relationship_id':relationship['relationship_id'],'reason':reason});continue
        selected.append({'relationship_id':relationship['relationship_id'],'artifact_sha256':relationship['artifact_sha256'],'type':typ,'category':category,'venue':venue,'proof_status':relationship['proof_status'],'market_ids':[k[1] for k in proposed]});markets.update(proposed);counts[(venue,category)]+=1;seen_types[(venue,typ)]+=1
    write(out/'requests.json',requests);write(out/'selection_rejections.json',rejected)
    byvenue={v:[m['market_id'] for (venue,_),m in markets.items() if venue==v] for v in ['kalshi','polymarket_international']}
    plan={'status':'READY' if all(byvenue.values()) else 'INSUFFICIENT_TWO_VENUE_TARGETS','selected_relationships':selected,'capture_market_ids':byvenue,'remaining_close_guard_sec':2100,'capture_duration_sec':1200,'data_scope':'REST preflight only, not synchronized L2','approval_status':'PENDING_HUMAN_REVIEW','rules_file':str(proofs),'rules_file_sha256':hashlib.sha256(proofs.read_bytes()).hexdigest(),'live_orders_sent':0}
    write(out/'capture_plan.json',plan)
    if plan['status']!='READY':return plan
    cfg.update(discovery_limit=200,page_size=100,capture_markets_per_venue=12,diagnostic_reconnect_after_sec=600,capture_market_ids=byvenue,diagnostic_watch_pairs=[],generalized_relationships=str(proofs),generalized_relationships_sha256=plan['rules_file_sha256'],human_review_manifest=str(proofs.parent/'approval_manifest.pending.json'))
    cfg.pop('research_seed_queries',None);cfg['research_seed_markets']=[]
    for (v,_),m in markets.items():cfg['research_seed_markets'].append({'venue':v,'metadata_url':'https://external-api.kalshi.com/trade-api/v2/markets/'+m['market_id'] if v=='kalshi' else 'https://gamma-api.polymarket.com/markets/'+m['raw']['id']})
    write(config_path,cfg);return plan
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--proofs',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--config-out',type=Path,required=True);a=p.parse_args();result=prepare(a.proofs,a.out,a.config_out);print(json.dumps(result,ensure_ascii=False))

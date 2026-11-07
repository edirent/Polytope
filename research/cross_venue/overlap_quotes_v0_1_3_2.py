"""All-candidate public order-book verification, before economic screening."""
import argparse,gzip,hashlib,json,sqlite3,subprocess,time
from collections import Counter,defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.parse import urlencode
from overlap_collect_v0_1_3_2 import PublicEvidence,write
from overlap_match_v0_1_3_2 import outcome_ids,norm,participants,configure_teams
ROOT=Path(__file__).resolve().parents[2]
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def csvout(path,rows,fields=None):
    import csv
    with path.open('w',newline='') as f:
        w=csv.DictWriter(f,fields or sorted(set().union(*(set(r) for r in rows))));w.writeheader()
        for row in rows:w.writerow({k:json.dumps(v,separators=(',',':')) if isinstance(v,(dict,list)) else v for k,v in row.items()})

def verify(run,workers=6):
    assert (run/'matching_manifest.json').is_file();stage=run/'quote_stage';stage.mkdir(exist_ok=True);client=PublicEvidence(stage/'responses');db=sqlite3.connect(stage/'quotes.sqlite',check_same_thread=False);db.execute('CREATE TABLE IF NOT EXISTS evidence (key TEXT PRIMARY KEY, kind TEXT, payload TEXT, source TEXT, status INTEGER)');db.commit();db_lock=__import__('threading').Lock()
    def cached(key):
        with db_lock:row=db.execute('SELECT payload,source,status FROM evidence WHERE key=?',(key,)).fetchone()
        return (json.loads(row[0]),json.loads(row[1]),row[2]) if row else None
    def put(key,kind,payload,source,status):
        with db_lock:db.execute('INSERT OR REPLACE INTO evidence VALUES (?,?,?,?,?)',(key,kind,json.dumps(payload),json.dumps(source),status));db.commit()
    candidates=[];needed=set()
    for line in (run/'candidate_matching_evidence.jsonl').open():
        p=json.loads(line);candidates.append(p);needed.update([p['kalshi_uid'],p['polymarket_uid']])
    markets={}
    for line in (run/'canonical_market_universe.jsonl').open():
        m=json.loads(line)
        if m['uid'] in needed:markets[m['uid']]=m
    keys={'kalshi':[uid for uid in sorted(needed) if uid.startswith('K:')],'polymarket_international':[uid for uid in sorted(needed) if uid.startswith('P:')]};print(json.dumps({'stage':'QUOTE_ALL_MATCHED_MARKETS','pairs':len(candidates),'unique_markets':{k:len(v) for k,v in keys.items()}}),flush=True)
    # Refresh explicit IDs only. Refreshed observations never overwrite frozen universe.
    def metadata_batch(venue,uids):
        missing=[uid for uid in uids if not cached('META:'+uid)]
        if not missing:return
        ids=[uid[2:] for uid in missing]
        params=[('limit',1000),('tickers',','.join(ids))] if venue=='kalshi' else [('limit',100),('closed','any'),('version','v1'),('version','v2')]+[('id',identifier) for identifier in ids]
        url=('https://external-api.kalshi.com/trade-api/v2/markets' if venue=='kalshi' else 'https://gamma-api.polymarket.com/markets/keyset')+'?'+urlencode(params);data,logs=client.request(url);found={str(m.get('ticker') if venue=='kalshi' else m.get('id')):m for m in (data or {}).get('markets',[]) if isinstance(m,dict)} if isinstance(data,dict) else {}
        for uid in missing:
            if uid[2:] in found:put('META:'+uid,'metadata',found[uid[2:]],logs[-1],200)
            else:
                u=('https://external-api.kalshi.com/trade-api/v2/markets/' if venue=='kalshi' else 'https://gamma-api.polymarket.com/markets/')+uid[2:];single,ls=client.request(u);raw=(single or {}).get('market') if venue=='kalshi' else single;put('META:'+uid,'metadata',raw if isinstance(raw,dict) else None,ls[-1],ls[-1]['http_status'])
    jobs=[(venue,uids[i:i+100]) for venue,uids in keys.items() for i in range(0,len(uids),100)]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for done,_ in enumerate(pool.map(lambda x:metadata_batch(*x),jobs),1):
            if done%50==0:print('metadata batches',done,'/',len(jobs),flush=True)
    refreshed={uid:cached('META:'+uid) for uid in needed};assets={};pm_specs={}
    for uid in keys['polymarket_international']:
        raw,source,status=refreshed[uid];raw=raw or {};version,outcomes,supported=outcome_ids(raw,markets[uid]['partition']);pm_specs[uid]={'version':version,'outcomes':outcomes,'supported':supported}
        for outcome in outcomes:
            if supported:assets[outcome['asset_id']]=uid
    def kalshi_books(uids):
        missing=[uid for uid in uids if not cached('BOOK:'+uid)]
        if not missing:return
        url='https://external-api.kalshi.com/trade-api/v2/markets/orderbooks?'+urlencode([('tickers',uid[2:]) for uid in missing]);data,logs=client.request(url);found={x['ticker']:x for x in (data or {}).get('orderbooks',[]) if isinstance(x,dict) and 'ticker' in x} if isinstance(data,dict) else {}
        for uid in missing:
            if uid[2:] in found:put('BOOK:'+uid,'kalshi_book',found[uid[2:]],logs[-1],200)
            else:
                data,ls=client.request('https://external-api.kalshi.com/trade-api/v2/markets/'+uid[2:]+'/orderbook?depth=0');put('BOOK:'+uid,'kalshi_book',data,ls[-1],ls[-1]['http_status'])
    def poly_books(ids):
        ids=[i for i in ids if not cached('ASSET:'+i)]
        if not ids:return
        data,logs=client.request('https://clob.polymarket.com/books','POST',[{'token_id':i} for i in ids]);found={str(x.get('asset_id')):x for x in data if isinstance(x,dict) and x.get('asset_id')} if isinstance(data,list) else {}
        for identifier in ids:
            if identifier in found:put('ASSET:'+identifier,'poly_book',found[identifier],logs[-1],200)
            elif len(ids)>1 and not found:
                # Missing/invalid assets must not invalidate the whole batch.
                mid=len(ids)//2;poly_books(ids[:mid]);poly_books(ids[mid:]);return
            else:
                d,ls=client.request('https://clob.polymarket.com/book?token_id='+identifier);put('ASSET:'+identifier,'poly_book',d,ls[-1],ls[-1]['http_status'])
    jobs=[('k',keys['kalshi'][i:i+100]) for i in range(0,len(keys['kalshi']),100)]+[('p',list(assets)[i:i+100]) for i in range(0,len(assets),100)]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for done,_ in enumerate(pool.map(lambda x:kalshi_books(x[1]) if x[0]=='k' else poly_books(x[1]),jobs),1):
            if done%50==0:print('book batches',done,'/',len(jobs),flush=True)
    inputs=stage/'native_quote_inputs.jsonl';book_sources={};input_objects={}
    with inputs.open('w') as f:
        for uid in keys['kalshi']:
            payload,source,status=cached('BOOK:'+uid)
            for side in ['YES','NO']:
                key=uid+':'+side;book_sources[key]={'source':source,'status':status,'payload':payload,'metadata':refreshed[uid][0]}
                if status!=200 or not isinstance(payload,dict) or not isinstance(payload.get('orderbook_fp'),dict):continue
                fp=dict(payload['orderbook_fp']);compat=False
                for name in ['yes_dollars','no_dollars']:
                    if fp.get(name) is None:fp[name]=[];compat=True
                packet={'request_id':key,'venue':'kalshi','market_id':uid[2:],'instrument_id':side,'received_wall_ns':source['received_wall_ns'],'received_monotonic_ns':source['received_monotonic_ns'],'raw_payload':json.dumps({'orderbook_fp':fp},separators=(',',':'))};input_objects[key]=packet;book_sources[key]['empty_side_materialized']=compat;f.write(json.dumps(packet,separators=(',',':'))+'\n')
        for identifier,uid in assets.items():
            payload,source,status=cached('ASSET:'+identifier);key='A:'+identifier;book_sources[key]={'source':source,'status':status,'payload':payload,'metadata':refreshed[uid][0]}
            if status!=200 or not isinstance(payload,dict):continue
            expected=(refreshed[uid][0] or {}).get('conditionId');actual=payload.get('market');book_sources[key]['market_binding_verified']=bool(expected and actual==expected and str(payload.get('asset_id'))==identifier)
            if not book_sources[key]['market_binding_verified']:continue
            packet={'request_id':key,'venue':'polymarket_international','market_id':expected,'instrument_id':identifier,'received_wall_ns':source['received_wall_ns'],'received_monotonic_ns':source['received_monotonic_ns'],'raw_payload':json.dumps(payload,separators=(',',':'))};input_objects[key]=packet;f.write(json.dumps(packet,separators=(',',':'))+'\n')
    normalized=stage/'normalized_rest_books.jsonl'
    if not normalized.exists():subprocess.run([str(ROOT/'build/cross_venue_overlap_inventory'),'--phase','books','--input',str(inputs),'--out',str(normalized)],cwd=ROOT,check=True)
    books={x['request_id']:x for x in map(json.loads,normalized.open())};configure_teams(json.loads((run/'supporting/all_teams.json').read_text()))
    def aligned_indices(k,p,spec):
        out=spec['outcomes'];labels=[norm(x['label']) for x in out]
        if len(out)!=2:return None,None,'NONBINARY_OR_MISSING_OUTCOME_MAPPING'
        if set(labels)=={'yes','no'}:return labels.index('yes'),labels.index('no'),'LITERAL_YES_NO'
        if set(labels)=={'up','down'}:return labels.index('up'),labels.index('down'),'UP_DOWN_EXPLICIT_ALIASES_NOT_SETTLEMENT_EQUIVALENCE'
        for i,label in enumerate(labels):
            names=participants(k['sport_or_competition'],label)
            if k.get('yes_outcome_entity') and k['yes_outcome_entity'] in names:return i,1-i,'PARTICIPANT_LABEL_ALIGNED_TO_KALSHI_YES'
            if norm(k.get('subtitle',''))==label and label not in ['','yes','no']:return i,1-i,'EXACT_OUTCOME_LABEL_ALIGNED'
        return None,None,'OUTCOME_ALIGNMENT_UNRESOLVED'
    inventory=[];verification=[];status_counts=Counter()
    def bk(key):return books.get(key,{})
    for pair in candidates:
        k,p=markets[pair['kalshi_uid']],markets[pair['polymarket_uid']];ku,pu=k['uid'],p['uid'];km,ks,kstatus=refreshed[ku];pm,ps,pstatus=refreshed[pu];spec=pm_specs[pu];yi,ni,alignment=aligned_indices(k,p,spec);kkeys=[ku+':YES',ku+':NO'];pkeys=['A:'+x['asset_id'] for x in spec['outcomes'] if x['asset_id']];py=pkeys[yi] if yi is not None else None;pn=pkeys[ni] if ni is not None else None;kany=any(bk(key).get('buyable') for key in kkeys);pany=any(bk(key).get('buyable') for key in pkeys);allkeys=kkeys+pkeys
        known_not_tradable=bool(km and km.get('status') not in ['active','open']) or bool(pm and (pm.get('closed') or not pm.get('active') or pm.get('acceptingOrders') is False or pm.get('enableOrderBook') is False));incomplete=any(key not in books and book_sources.get(key,{}).get('status')!=404 for key in allkeys) or kstatus!=200 or pstatus!=200
        if known_not_tradable:quote_status='NOT_CURRENTLY_TRADABLE'
        elif not spec['supported']:quote_status='UNSUPPORTED_PROTOCOL'
        elif kany and pany:quote_status='BOTH_VENUES_QUOTED'
        elif incomplete:quote_status='INCOMPLETE_BOOK_EVIDENCE'
        elif kany or pany:quote_status='ONE_VENUE_ONLY_QUOTED'
        else:quote_status='BOTH_VENUES_UNQUOTED'
        direction0=bool(bk(kkeys[0]).get('buyable') and bk(pn).get('buyable')) if pn else None;direction1=bool(bk(kkeys[1]).get('buyable') and bk(py).get('buyable')) if py else None;times=[book_sources[key]['source']['received_wall_ns'] for key in allkeys if key in book_sources];sources=[book_sources[key]['source'] for key in allkeys if key in book_sources]
        row={'pair_id':pair['pair_id'],'kalshi_market_id':k['market_id'],'kalshi_event_id':k['event_id'],'kalshi_title':k['title'],'kalshi_url':k['url'],'polymarket_market_id':p['market_id'],'polymarket_event_id':p['event_id'],'polymarket_condition_id':(pm or {}).get('conditionId',p['condition_id']),'polymarket_title':p['title'],'polymarket_url':p['url'],'polymarket_outcome_asset_ids':spec['outcomes'],'polymarket_protocol':spec['version'],'canonical_event':{'entities':k['entities'],'predicate':k['event_predicate'],'event_date':k['event_date'],'threshold':k['numerical_threshold'],'participants':k['participant_names']},'event_category':pair['event_category'],'match_classification':pair['match_classification'],'match_confidence':pair['match_confidence'],'match_evidence':{'methods':pair['matching_methods'],'features':pair['matching_features'],'conflicts':pair['conflicting_features']},'kalshi_market_status':(km or {}).get('status'),'polymarket_market_status':'closed' if (pm or {}).get('closed') else 'active' if (pm or {}).get('active') else 'unknown_or_inactive','kalshi_yes_bid':bk(kkeys[0]).get('bid'),'kalshi_yes_ask':bk(kkeys[0]).get('ask'),'kalshi_no_bid':bk(kkeys[1]).get('bid'),'kalshi_no_ask':bk(kkeys[1]).get('ask'),'polymarket_yes_bid':bk(py).get('bid'),'polymarket_yes_ask':bk(py).get('ask'),'polymarket_no_bid':bk(pn).get('bid'),'polymarket_no_ask':bk(pn).get('ask'),'kalshi_quote_sizes':{side:{'bid':bk(key).get('bid_size'),'ask':bk(key).get('ask_size')} for side,key in zip(['yes','no'],kkeys)},'polymarket_quote_sizes':{x['label']:{'bid':bk('A:'+x['asset_id']).get('bid_size'),'ask':bk('A:'+x['asset_id']).get('ask_size')} for x in spec['outcomes'] if x['asset_id']},'quote_status':quote_status,'quote_source':'ACTUAL_REST_ORDER_BOOKS_NOT_CONTINUOUS_SYNCHRONIZED_L2','quote_observation_time':{'first_wall_ns':min(times) if times else None,'last_wall_ns':max(times) if times else None},'quote_observation_skew_ms':(max(times)-min(times))/1e6 if times else None,'direction_0_buyable':direction0,'direction_1_buyable':direction1,'both_directions_buyable':direction0 and direction1 if direction0 is not None and direction1 is not None else None,'polymarket_yes_label':spec['outcomes'][yi]['label'] if yi is not None else None,'polymarket_no_label':spec['outcomes'][ni]['label'] if ni is not None else None,'outcome_alignment':alignment,'same_event_confidence':pair['match_confidence'],'rules_equivalence_status':'NOT_REVIEWED_DISCOVERY_INVENTORY','rules_mismatch_reasons':pair['conflicting_features'],'settlement_asset_status':'UNVERIFIED_CROSS_ASSET_CONVERSION','quantity_precision_status':'UNVERIFIED_MINIMUM_OR_STEP' if not all([k['minimum_quantity'],k['quantity_step'],p['minimum_quantity'],p['quantity_step']]) else 'METADATA_MINIMUM_AND_STEP_PRESENT','discovery_source_hashes':pair['source_hashes'],'rule_source_hashes':pair['source_rule_hashes'],'actual_book_source_hashes':[s['raw_sha256'] for s in sources],'quote_native_request_ids':{'kalshi_yes':kkeys[0],'kalshi_no':kkeys[1],'polymarket_yes':py,'polymarket_no':pn},'human_approved':False,'rules_revision':1,'current_ws_validated':False}
        inventory.append(row);status_counts[quote_status]+=1;verification.append({'pair_id':pair['pair_id'],'quote_status':quote_status,'direction_0_buyable':direction0,'direction_1_buyable':direction1,'outcome_alignment':alignment,'request_ids':allkeys,'sources':sources,'metadata_source_hashes':[ks['raw_sha256'],ps['raw_sha256']],'metadata_status_changed_during_collection':(km or {}).get('status')!=k['market_status'] if km else None,'current_ws_validated':False})
    csvout(run/'cross_venue_market_overlap.csv',inventory);csvout(run/'quote_verification.csv',verification);csvout(run/'matched_markets_with_quotes.csv',[r for r in inventory if r['quote_status']=='BOTH_VENUES_QUOTED'] or [{'status':'UNAVAILABLE','reason':'NO_BOTH_VENUES_QUOTED_CANDIDATES'}]);csvout(run/'matched_markets_without_quotes.csv',[r for r in inventory if r['quote_status']!='BOTH_VENUES_QUOTED'] or [{'status':'UNAVAILABLE','reason':'ALL_CANDIDATES_BOTH_VENUES_QUOTED'}]);write(stage/'inventory_frozen.json',{'stage':'OVERLAP_INVENTORY_FROZEN_BEFORE_ECONOMIC_SCREENING','pairs':len(inventory),'quote_status_counts':status_counts,'inventory_sha256':digest(run/'cross_venue_market_overlap.csv'),'matching_evidence_sha256':digest(run/'candidate_matching_evidence.jsonl'),'normalized_books_sha256':digest(normalized),'snapshot_window_non_atomic':True,'real_order_submission':False});db.close();print(json.dumps({'inventory_pairs':len(inventory),'quote_status_counts':status_counts}),flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--run-dir',type=Path,required=True);p.add_argument('--workers',type=int,default=6);a=p.parse_args();verify(a.run_dir.resolve(),a.workers)

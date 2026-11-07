"""Checkpointed public universe enumeration. No matching/economic filters or credentials."""
import argparse,gzip,hashlib,json,os,sqlite3,threading,time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.parse import urlencode,urlparse
import requests
ALLOWED={'external-api.kalshi.com','gamma-api.polymarket.com','clob.polymarket.com'}
def sha(b):return hashlib.sha256(b).hexdigest()
def write(p,obj):
    temp=p.with_suffix(p.suffix+'.tmp');temp.write_text(json.dumps(obj,indent=2)+'\n');temp.replace(p)
class PublicEvidence:
    def __init__(self,root):
        self.root=Path(root);self.root.mkdir(parents=True,exist_ok=True);self.local=threading.local();self.lock=threading.Lock();self.next={};self.counter=0
    def request(self,url,method='GET',body=None,attempts=4):
        host=urlparse(url).hostname
        if host not in ALLOWED or any(x in urlparse(url).path for x in ['/orders','/portfolio','/auth']):raise ValueError('not an allowed public market-data endpoint')
        if method not in ['GET','POST'] or (method=='POST' and urlparse(url).path!='/books'):raise ValueError('POST restricted to public batch order books')
        if not hasattr(self.local,'session'):self.local.session=requests.Session()
        logs=[]
        for attempt in range(attempts):
            with self.lock:
                delay=max(0,self.next.get(host,0)-time.monotonic());self.next[host]=time.monotonic()+delay+(.4 if host=='external-api.kalshi.com' else .06);number=self.counter;self.counter+=1
            if delay:time.sleep(delay)
            start=time.time_ns();mono=time.monotonic_ns();status=0;payload=b'';error=None;retry_after=0
            try:
                response=self.local.session.request(method,url,json=body if body is not None else None,headers={'User-Agent':'Polytope-readonly-overlap-v0.1.3.2','Accept':'application/json'},timeout=(10,30));status=response.status_code;payload=response.content
                try:retry_after=min(30,float(response.headers.get('Retry-After',0)))
                except ValueError:pass
            except requests.RequestException as e:error=type(e).__name__
            end=time.time_ns();name=f'{start}-{number:06d}.json.gz';path=self.root/name
            with path.open('wb') as f:
                with gzip.GzipFile(filename='',mode='wb',fileobj=f,mtime=0) as z:z.write(payload)
            entry={'url':url,'method':method,'request_body':body,'http_status':status,'attempt':attempt,'started_wall_ns':start,'received_wall_ns':end,'started_monotonic_ns':mono,'received_monotonic_ns':time.monotonic_ns(),'raw_file':str(path),'raw_sha256':sha(payload),'stored_sha256':sha(path.read_bytes()),'error':error,'retry_after_seconds':retry_after};logs.append(entry)
            with self.lock:
                with (self.root/'requests.jsonl').open('a') as f:f.write(json.dumps(entry)+'\n')
            if status==200:
                try:return response.json(),logs
                except ValueError:error='NON_JSON_200'
            if status not in [0,408,425,429,500,502,503,504]:break
            time.sleep(max(retry_after,min(8,2**attempt)))
        return None,logs

def enumerate_partition(run,name,url,query,*,budget_seconds=None,client=None):
    directory=run/'enumeration'/name;directory.mkdir(parents=True,exist_ok=True);checkpoint=directory/'checkpoint.json';db=sqlite3.connect(directory/'markets.sqlite');db.execute('CREATE TABLE IF NOT EXISTS markets (id TEXT PRIMARY KEY, raw TEXT, source TEXT, first_source TEXT, versions INTEGER DEFAULT 1)');db.commit()
    state=json.loads(checkpoint.read_text()) if checkpoint.exists() else {'partition':name,'base_url':url,'filters':query,'cursor':None,'pages':0,'raw_records':0,'duplicates':0,'changed_duplicates':0,'started_wall_ns':time.time_ns(),'complete':False,'termination_evidence':None,'failed_pages':[],'pagination':[]}
    assert state['base_url']==url and state['filters']==query
    if state['complete']:db.close();return state
    client=client or PublicEvidence(directory/'responses');start=time.monotonic();seen_cursors={p['cursor_in'] for p in state['pagination'] if p.get('cursor_in')}
    while not state['complete']:
        if budget_seconds and time.monotonic()-start>=budget_seconds:state['stop_reason']='EXPLICIT_COMBO_TIME_BUDGET_CHECKPOINT_RETAINED';break
        params=list(query);cursor=state['cursor']
        if cursor:params.append(('after_cursor' if 'polymarket' in name else 'cursor',cursor))
        page_url=url+'?'+urlencode(params);data,requests_log=client.request(page_url)
        if data is None:
            state['failed_pages'].append({'url':page_url,'cursor':cursor,'requests':requests_log});state['stop_reason']='PAGE_UNAVAILABLE_AFTER_BOUNDED_RETRIES';break
        if not isinstance(data,dict) or not isinstance(data.get('markets'),list):state['stop_reason']='UNEXPECTED_RESPONSE_SCHEMA';state['failed_pages'].append({'url':page_url,'schema_type':type(data).__name__});break
        values=data['markets'];next_cursor=data.get('next_cursor') if 'polymarket' in name else data.get('cursor');source=requests_log[-1]
        for market in values:
            identifier=market.get('ticker') if name.startswith('kalshi') else market.get('id')
            if identifier is None:raise ValueError('market without stable identifier; retain page and stop instead of discarding')
            identifier=str(identifier);serialized=json.dumps(market,separators=(',',':'));old=db.execute('SELECT raw FROM markets WHERE id=?',(identifier,)).fetchone()
            if old:
                state['duplicates']+=1;state['changed_duplicates']+=old[0]!=serialized;db.execute('UPDATE markets SET raw=?,source=?,versions=versions+1 WHERE id=?',(serialized,json.dumps(source),identifier))
            else:db.execute('INSERT INTO markets (id,raw,source,first_source) VALUES (?,?,?,?)',(identifier,serialized,json.dumps(source),json.dumps(source)))
        db.commit();state['pages']+=1;state['raw_records']+=len(values);state['pagination'].append({'page':state['pages'],'cursor_in':cursor,'cursor_out':next_cursor,'returned':len(values),'url':page_url,'raw_sha256':source['raw_sha256'],'received_wall_ns':source['received_wall_ns']});state['cursor']=next_cursor;state['last_wall_ns']=source['received_wall_ns'];state['unique_market_ids']=db.execute('SELECT COUNT(*) FROM markets').fetchone()[0]
        if not next_cursor:state['complete']=True;state['termination_evidence']={'reason':'ACTUAL_TERMINAL_CURSOR','terminal_page':state['pagination'][-1]};state['stop_reason']='TERMINAL_CURSOR_OBSERVED'
        elif next_cursor in seen_cursors or next_cursor==cursor:state['stop_reason']='REPEATED_CURSOR_NOT_COMPLETION';write(checkpoint,state);break
        seen_cursors.add(cursor);write(checkpoint,state)
        if state['pages']%20==0:print(json.dumps({'partition':name,'pages':state['pages'],'unique_markets':state['unique_market_ids'],'complete':state['complete']}),flush=True)
    state['unique_market_ids']=db.execute('SELECT COUNT(*) FROM markets').fetchone()[0];state['non_atomic_snapshot']=True;write(checkpoint,state)
    out=directory/'markets.jsonl'
    with out.open('w') as stream:
        for identifier,raw,source,first,versions in db.execute('SELECT id,raw,source,first_source,versions FROM markets ORDER BY id'):
            stream.write(json.dumps({'venue':'kalshi' if name.startswith('kalshi') else 'polymarket_international','partition':name,'id':identifier,'raw':json.loads(raw),'source':json.loads(source),'first_source':json.loads(first),'observed_versions':versions},separators=(',',':'))+'\n')
    db.close();print(json.dumps({'partition':name,'pages':state['pages'],'unique_markets':state['unique_market_ids'],'complete':state['complete'],'stop_reason':state['stop_reason']}),flush=True);return state

def collect(run,combo_budget=180):
    run.mkdir(parents=True,exist_ok=True)
    tasks=[('kalshi_standard','https://external-api.kalshi.com/trade-api/v2/markets',[('limit',1000),('status','open'),('mve_filter','exclude')],None),('polymarket_v1','https://gamma-api.polymarket.com/markets/keyset',[('limit',500),('closed','false'),('version','v1')],None),('polymarket_v2','https://gamma-api.polymarket.com/markets/keyset',[('limit',500),('closed','false'),('version','v2')],None),('kalshi_combos','https://external-api.kalshi.com/trade-api/v2/markets',[('limit',1000),('status','open'),('mve_filter','only')],combo_budget)]
    client=PublicEvidence(run/'enumeration/responses')
    with ThreadPoolExecutor(max_workers=4) as pool:
        futures=[pool.submit(enumerate_partition,run,n,u,q,budget_seconds=b,client=client) for n,u,q,b in tasks]
        results=[f.result() for f in futures]
    for venue,partitions,filename in [('kalshi',['kalshi_standard','kalshi_combos'],'kalshi_all_open_markets.jsonl'),('polymarket_international',['polymarket_v1','polymarket_v2'],'polymarket_all_open_markets.jsonl')]:
        seen=set();duplicates=0
        with (run/filename).open('w') as out:
            for name in partitions:
                for line in (run/'enumeration'/name/'markets.jsonl').open():
                    record=json.loads(line);key=record['id']
                    if key in seen:duplicates+=1;continue
                    seen.add(key);out.write(line)
        selected=[x for x in results if x['partition'] in partitions];write(run/(('kalshi' if venue=='kalshi' else 'polymarket')+'_discovery_manifest.json'),{'schema':'exhaustive-accessible-universe-v0.1.3.2','venue':venue,'partitions':selected,'unique_market_ids':len(seen),'cross_partition_duplicate_ids':duplicates,'all_partitions_terminal':all(x['complete'] for x in selected),'no_liquidity_volume_holding_period_or_spread_filters':True,'collection_is_not_atomic':True,'universe_sha256':sha((run/filename).read_bytes()),'live_orders_sent':0})
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--run-dir',type=Path,required=True);p.add_argument('--combo-budget-sec',type=int,default=180);a=p.parse_args();collect(a.run_dir.resolve(),a.combo_budget_sec)

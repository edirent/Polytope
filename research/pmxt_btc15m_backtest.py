#!/usr/bin/env python3
"""Audited PMXT v2 BTC15m research pipeline. Public data only; no order transport."""
from __future__ import annotations
import argparse
import concurrent.futures as futures
import datetime as dt
import hashlib
import io
import json
import subprocess
import time
import zipfile
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
import requests

UTC=dt.timezone.utc
PMXT='https://r2v2.pmxt.dev/'
FEATURES=['btc_100ms','btc_250ms','btc_1s','btc_5s','imbalance_250ms','pm_500ms','pm_1s','pm_5s','pair_gap','token_spread','anchor','tau_seconds','token_deviation']


def save(path, value):
    path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_text(json.dumps(value,indent=2)+'\n');tmp.replace(path)


def get(session, url, **kwargs):
    for attempt in range(4):
        try:
            r=session.get(url,timeout=(15,60),**kwargs);r.raise_for_status();return r
        except requests.RequestException:
            if attempt==3:raise
            time.sleep(min(2**attempt,5))


def audit_inputs(out,workers,start_date='2026-04-17',end_date='2026-05-01'):
    """Audit every requested hour directly; archive index continuity is never assumed."""
    out.mkdir(parents=True,exist_ok=True);(out/'gamma').mkdir(exist_ok=True)
    begin=dt.datetime.fromisoformat(start_date).replace(tzinfo=UTC);end=dt.datetime.fromisoformat(end_date).replace(tzinfo=UTC)
    if end<=begin or begin.hour or end.hour:raise ValueError('expected increasing UTC date boundaries')
    requested_hours=int((end-begin).total_seconds()//3600)
    hours=[begin-dt.timedelta(days=1)+dt.timedelta(hours=i) for i in range(requested_hours+25)]
    def head(stamp):
        url=PMXT+'polymarket_orderbook_'+stamp.strftime('%Y-%m-%dT%H')+'.parquet'
        try:
            r=requests.head(url,timeout=(15,45))
            return {'hour_utc':stamp.isoformat(),'url':url,'status':r.status_code,
                'size_bytes':int(r.headers.get('Content-Length',0)),'etag':r.headers.get('ETag'),'available':r.status_code==200}
        except requests.RequestException as e:return {'hour_utc':stamp.isoformat(),'url':url,'available':False,'error':str(e)}
    with futures.ThreadPoolExecutor(max_workers=workers) as pool:coverage=list(pool.map(head,hours))
    save(out/'hourly_coverage.json',coverage)
    requested=[r for r in coverage if begin<=dt.datetime.fromisoformat(r['hour_utc'])<end]
    missing=[r for r in requested if not r['available']]
    save(out/'coverage_summary.json',{'requested_start_utc':begin.isoformat(),'requested_end_exclusive_utc':end.isoformat(),
        'expected_hours':requested_hours,'available_hours':sum(r['available'] for r in requested),'missing_or_unverified':missing,
        'total_object_bytes':sum(r.get('size_bytes',0) for r in requested),'prior_day_available_hours':sum(r['available'] for r in coverage[:24])})
    def market(i):
        start=int(begin.timestamp())+i*900;slug='btc-updown-15m-'+str(start);path=out/'gamma'/(str(start)+'.json')
        g=json.loads(path.read_text()) if path.exists() else get(requests.Session(),'https://gamma-api.polymarket.com/events/slug/'+slug).json()
        if not path.exists():save(path,g)
        candidates=[m for m in g['markets'] if m.get('slug')==slug]
        if len(candidates)!=1:raise ValueError('ambiguous market '+slug)
        m=candidates[0];outcomes=json.loads(m['outcomes']);tokens=json.loads(m['clobTokenIds']);prices=json.loads(m['outcomePrices'])
        if not m.get('closed') or sorted(outcomes)!=['Down','Up'] or sorted(float(p) for p in prices)!=[0.,1.]:
            raise ValueError('missing binary settlement '+slug)
        if float(m.get('orderMinSize',0))!=5:raise ValueError('unsupported venue minimum '+slug)
        return {'slug':slug,'condition_id':m['conditionId'],'up_token':tokens[outcomes.index('Up')],
            'down_token':tokens[outcomes.index('Down')],'start_ms':start*1000,'expiry_ms':(start+900)*1000,
            'created_at':m.get('createdAt',g['creationDate']),'closed':True,
            'winner':outcomes[next(j for j,p in enumerate(prices) if float(p)==1)],'question':m['question']}
    with futures.ThreadPoolExecutor(max_workers=workers) as pool:markets=list(pool.map(market,range(requested_hours*4)))
    save(out/'markets.json',markets)
    if missing:raise RuntimeError('archive coverage gaps; see coverage_summary.json before proceeding')


class RangeFile(io.RawIOBase):
    """Read exact HTTP byte ranges; refuses a server silently returning whole files."""
    def __init__(self,url):
        self.url=url;self.session=requests.Session();self.pos=0;self.transferred=0;self.calls=0;self.cache=[]
        r=get(self.session,url,headers={'Range':'bytes=-8'})
        if r.status_code!=206 or r.content[-4:]!=b'PAR1':raise ValueError('not a range-readable Parquet object')
        self.length=int(r.headers['Content-Range'].split('/')[-1]);self.etag=r.headers.get('ETag')
    def readable(self):return True
    def seekable(self):return True
    def tell(self):return self.pos
    def seek(self,p,whence=0):
        self.pos=p if whence==0 else self.pos+p if whence==1 else self.length+p
        if self.pos<0:raise ValueError('negative seek')
        return self.pos
    def fetch(self,start,size):
        r=get(self.session,self.url,headers={'Range':f'bytes={start}-{start+size-1}'})
        if r.status_code!=206 or len(r.content)!=size or r.headers.get('ETag')!=self.etag:
            raise ValueError('archive changed or byte range was not honored')
        self.transferred+=size;self.calls+=1;return r.content
    def read(self,size=-1):
        size=self.length-self.pos if size<0 else min(size,self.length-self.pos)
        if size<=0:return b''
        start=self.pos
        for a,data in reversed(self.cache):
            if a<=start and start+size<=a+len(data):
                self.pos+=size;return data[start-a:start-a+size]
        data=self.fetch(start,size);self.pos+=size
        # Footer and selected row group bytes only, bounded per worker.
        return data
    def prefetch_group(self,group):
        starts=[];ends=[]
        for i in range(group.num_columns):
            c=group.column(i)
            start=min(x for x in (c.dictionary_page_offset,c.data_page_offset) if x is not None and x>=0)
            starts.append(start);ends.append(start+c.total_compressed_size)
        lo,hi=min(starts),max(ends)
        self.cache=[(lo,self.fetch(lo,hi-lo))]


def fetch_hours(out,workers):
    markets=[m for m in json.loads((out/'markets.json').read_text()) if 'error' not in m]
    coverage=json.loads((out/'hourly_coverage.json').read_text())
    (out/'filtered').mkdir(exist_ok=True)
    def fetch_hour(row):
        stamp=dt.datetime.fromisoformat(row['hour_utc']); hour=int(stamp.timestamp()*1000)
        target=[]
        for m in markets:
            created=int(dt.datetime.fromisoformat(m['created_at'].replace('Z','+00:00')).timestamp()*1000)
            # Include initial subscription hour, its following hour, live
            # contract hour and preceding hour. Do not scan all condition IDs.
            if hour<=created<hour+3_600_000 or hour<=created+3_600_000<hour+3_600_000 or \
               m['start_ms']-3_600_000<=hour<m['expiry_ms']+3_600_000:
                target.append(m['condition_id'].encode())
        key=stamp.strftime('%Y-%m-%dT%H')
        path=out/'filtered'/(key+'.parquet');audit_path=path.with_suffix('.audit.json')
        if path.exists() and audit_path.exists():return json.loads(audit_path.read_text())
        if not row['available'] or not target:
            return {'hour':key,'skipped':True,'reason':'unavailable' if not row['available'] else 'no_target_markets'}
        f=RangeFile(row['url']);parquet=pq.ParquetFile(f)
        if f.etag!=row.get('etag') or f.length!=row.get('size_bytes'):
            raise ValueError('archive object changed since coverage audit: '+key)
        names=parquet.schema_arrow.names
        required={'timestamp_received','timestamp','market','event_type','asset_id','price','size','side'}
        if not required.issubset(names):raise ValueError(f'{key}: incompatible schema {names}')
        groups=[]
        market_index=names.index('market')
        for i in range(parquet.metadata.num_row_groups):
            stats=parquet.metadata.row_group(i).column(market_index).statistics
            if stats is None or not stats.has_min_max or any(stats.min<=v<=stats.max for v in target):groups.append(i)
        selected=[]
        ids=pa.array(target,type=parquet.schema_arrow.field('market').type)
        for group_index in groups:
            f.prefetch_group(parquet.metadata.row_group(group_index))
            table=parquet.read_row_group(group_index,use_threads=False)
            table=table.filter(pc.is_in(table['market'],value_set=ids))
            if table.num_rows:selected.append(table)
        table=pa.concat_tables(selected) if selected else pa.Table.from_batches([],schema=parquet.schema_arrow)
        tmp=path.with_suffix('.tmp');pq.write_table(table,tmp,compression='zstd');tmp.replace(path)
        recv=pc.cast(table['timestamp_received'],pa.int64()).to_numpy()
        source=pc.cast(table['timestamp'],pa.int64()).to_numpy()
        lag=recv-source
        audit={'hour':key,'source_url':row['url'],'etag':f.etag,'object_bytes':f.length,'downloaded_bytes':f.transferred,
               'row_groups_read':len(groups),'row_groups_total':parquet.metadata.num_row_groups,'archive_rows':parquet.metadata.num_rows,
               'selected_rows':table.num_rows,'schema':str(parquet.schema_arrow),'negative_lag_rows':int(np.sum(lag<0)),
               'lag_ms_quantiles':np.quantile(lag,[0,.5,.95,.99,1]).tolist() if len(lag) else [],
               'selected_sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        save(audit_path,audit);return audit
    results=[]
    with futures.ThreadPoolExecutor(max_workers=workers) as pool:
        jobs={pool.submit(fetch_hour,row):row for row in coverage}
        for job in futures.as_completed(jobs):
            row=jobs[job]
            try:r=job.result()
            except Exception as e:r={'hour':row['hour_utc'],'error':str(e)}
            results.append(r)
            if len(results)%10==0:print(f'PMXT hours {len(results)}/{len(coverage)}, filtered rows {sum(x.get("selected_rows",0) for x in results):,}',flush=True)
    results.sort(key=lambda r:r['hour']);save(out/'extraction_audit.json',results)
    errors=[r for r in results if 'error'in r]
    print('PMXT complete',len(results),'errors',len(errors),'bytes',sum(r.get('downloaded_bytes',0) for r in results),flush=True)
    if errors:raise RuntimeError(f'{len(errors)} hours failed; see extraction_audit.json')


def fetch_binance(out,workers,start_date='2026-04-17',end_date='2026-05-01'):
    (out/'binance').mkdir(exist_ok=True)
    start=dt.datetime.fromisoformat(start_date).replace(tzinfo=UTC)-dt.timedelta(days=1)
    end=dt.datetime.fromisoformat(end_date).replace(tzinfo=UTC)
    dates=[(start+dt.timedelta(days=i)).strftime('%Y-%m-%d') for i in range((end-start).days+1)]
    def one(day):
        path=out/'binance'/(day+'.parquet');audit_path=path.with_suffix('.audit.json')
        if path.exists() and audit_path.exists():return json.loads(audit_path.read_text())
        url=f'https://data.binance.vision/data/spot/daily/aggTrades/BTCUSDT/BTCUSDT-aggTrades-{day}.zip'
        session=requests.Session();raw=get(session,url).content
        expected=get(session,url+'.CHECKSUM').text.split()[0]
        if hashlib.sha256(raw).hexdigest()!=expected:raise ValueError('Binance checksum mismatch '+day)
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            import pyarrow.csv as csv
            table=csv.read_csv(z.open(z.namelist()[0]),read_options=csv.ReadOptions(column_names=['id','price','quantity','first','last','source_us','buyer_is_maker','best_match']),
                convert_options=csv.ConvertOptions(column_types={'id':pa.int64(),'price':pa.float64(),'quantity':pa.float64(),'first':pa.int64(),'last':pa.int64(),'source_us':pa.int64(),'buyer_is_maker':pa.bool_(),'best_match':pa.bool_()}))
        source=table['source_us'].to_numpy();
        if source.min()<1_000_000_000_000_000:raise ValueError('expected Binance microsecond source timestamps')
        normalized=pa.table({'event_ms':source//1000,'price':table['price'],'quantity':table['quantity'],'buyer_is_maker':table['buyer_is_maker']})
        pq.write_table(normalized,path,compression='zstd',row_group_size=100_000)
        audit={'day':day,'url':url,'zip_sha256':expected,'rows':len(normalized),'source_start_ms':int(source.min()//1000),'source_end_ms':int(source.max()//1000),'has_measured_receive_clock':False}
        save(audit_path,audit);return audit
    with futures.ThreadPoolExecutor(max_workers=min(workers,4)) as pool:results=list(pool.map(one,dates))
    save(out/'binance_audit.json',results);print('Binance complete',len(results),'days',sum(r['rows'] for r in results),'aggTrades',flush=True)


def shard_hours(out):
    market_map={m['condition_id'].encode():m for m in json.loads((out/'markets.json').read_text()) if 'error' not in m}
    paths=sorted((out/'filtered').glob('*.parquet'))
    for n,path in enumerate(paths):
        marker=path.with_suffix('.sharded')
        if marker.exists():continue
        table=pq.read_table(path)
        if len(table):
            ids=table['market'].to_numpy()
            boundaries=np.r_[0,np.flatnonzero(ids[1:]!=ids[:-1])+1,len(ids)]
            for a,b in zip(boundaries[:-1],boundaries[1:]):
                m=market_map.get(ids[a])
                if m is None:continue
                dest=out/'market_rows'/m['slug'];dest.mkdir(parents=True,exist_ok=True)
                pq.write_table(table.slice(int(a),int(b-a)),dest/path.name,compression='zstd')
        marker.write_text('complete\n')
        if (n+1)%20==0:print('sharded',n+1,'/',len(paths),flush=True)


def _column(table,name,numeric=False):
    if name not in table.column_names:return np.full(len(table),np.nan if numeric else None)
    c=table[name]
    if numeric:c=pc.cast(c,pa.float64())
    return c.to_numpy(zero_copy_only=False)


def _load_btc(out,start,end,delay=20):
    days=[]
    lo=dt.datetime.fromtimestamp((start-65_000)/1000,UTC).date()
    hi=dt.datetime.fromtimestamp((end+5_000)/1000,UTC).date()
    day=lo
    while day<=hi:
        path=out/'binance'/(day.isoformat()+'.parquet')
        t=pq.read_table(path,filters=[('event_ms','>=',start-65_000),('event_ms','<=',end+5_000)])
        if len(t):days.append(t)
        day+=dt.timedelta(days=1)
    t=pa.concat_tables(days)
    source=_column(t,'event_ms').astype(np.int64)
    return source,source+delay,_column(t,'price'),_column(t,'quantity'),_column(t,'buyer_is_maker').astype(bool)


def build_features(times,tops,usable,btc,expiry,pm_only=False):
    """Vectorized equivalent of recv-clock-rv1s-60s-v1; checked against C++."""
    from statistics import NormalDist
    source,recv,price,quantity,maker=btc
    log_price=np.log(price)
    up=(tops[:,0,0]+tops[:,0,1])/2/1e6
    down=1-(tops[:,1,0]+tops[:,1,1])/2/1e6
    anchor=(up+down)/2
    ok=usable.copy()
    x=np.zeros((len(times),2,13))
    if not pm_only and len(price)==0:return x,np.zeros(len(times),dtype=bool)
    if not pm_only:
        def btc_lag(target):
            index=np.searchsorted(recv,target,side='right')-1
            safe=np.maximum(index,0)
            valid=(index>=0)&(target-recv[safe]<=1500)
            return log_price[safe],valid
        returns=[];valid=[]
        for i in range(60):
            a,va=btc_lag(times-i*1000);b,vb=btc_lag(times-(i+1)*1000)
            returns.append(a-b);valid.append(va&vb)
        r=np.asarray(returns).T;v=np.cumprod(np.asarray(valid).T,axis=1).astype(bool)
        count=v.sum(axis=1);s=(r*v).sum(axis=1);ss=(r*r*v).sum(axis=1)
        variance=np.divide(ss-np.divide(s*s,count,out=np.zeros_like(s),where=count>0),count-1,out=np.zeros_like(s),where=count>1)
        ok&=(count>=10)&(variance>1e-16)&(times<expiry)
        quantile=np.array([NormalDist().inv_cdf(min(.999,max(.001,float(p)))) for p in anchor])
        sensitivity=np.exp(-quantile**2/2)/np.sqrt(2*np.pi)/np.sqrt(np.maximum(variance,1e-16))/np.sqrt(np.maximum((expiry-times)/1000,1e-6))
        current,current_ok=btc_lag(times);ok&=current_ok
        for i,h in enumerate([100,250,1000,5000]):
            old,old_ok=btc_lag(times-h);ok&=old_ok
            x[:,:,i]=np.clip(sensitivity*(current-old),-.25,.25)[:,None]
        volume=np.r_[0,np.cumsum(quantity)];signed=np.r_[0,np.cumsum(quantity*np.where(maker,-1,1))]
        right=np.searchsorted(recv,times,side='right');left=np.searchsorted(recv,times-250,side='right')
        total=volume[right]-volume[left]
        imbalance=np.divide(signed[right]-signed[left],total,out=np.zeros(len(times)),where=total>0)
        x[:,:,4]=imbalance[:,None]
    pt=times[usable];pv=anchor[usable]
    if len(pt)==0:return x,np.zeros(len(times),dtype=bool)
    for i,h in enumerate([500,1000,5000]):
        idx=np.searchsorted(pt,times-h,side='right')-1;safe=np.maximum(idx,0)
        # Mirror C++'s 6s local-history retention, including one predecessor.
        oldest=np.maximum(np.searchsorted(pt,times-6000,side='left')-1,0)
        ok&=(idx>=oldest)&(idx>=0)&(times-h-pt[safe]<=1500)
        x[:,:,5+i]=(anchor-pv[safe])[:,None]
    x[:,:,8]=(up-down)[:,None]
    x[:,:,9]=(tops[:,:,1]-tops[:,:,0])/1e6
    x[:,:,10]=anchor[:,None];x[:,:,11]=((expiry-times)/1000)[:,None]
    x[:,0,12]=up-anchor;x[:,1,12]=down-anchor
    return x,ok


def normalize_market(job):
    from collections import Counter
    out,m=job;out=Path(out)
    dest=out/'normalized'/m['slug'];dest.mkdir(parents=True,exist_ok=True)
    audit_path=dest/'coverage.json'
    if audit_path.exists() and (dest/'features.npz').exists() and (dest/'events.jsonl').exists():return json.loads(audit_path.read_text())
    paths=sorted((out/'market_rows'/m['slug']).glob('*.parquet'))
    if not paths:return {'slug':m['slug'],'error':'no archived market rows'}
    table=pa.concat_tables([pq.read_table(p) for p in paths])
    recv=pc.cast(table['timestamp_received'],pa.int64()).to_numpy()
    src=pc.cast(table['timestamp'],pa.int64()).to_numpy()
    good=(recv>=src)&(recv<=m['expiry_ms']+5000)
    negative=int(np.sum(recv<src));table=table.filter(pa.array(good));recv=recv[good];src=src[good]
    idx=np.lexsort((np.arange(len(recv)),src,recv));table=table.take(pa.array(idx));recv=recv[idx];src=src[idx]
    kinds=_column(table,'event_type');assets=_column(table,'asset_id');sides=_column(table,'side')
    prices=_column(table,'price',True);sizes=_column(table,'size',True)
    bids=_column(table,'bids');asks=_column(table,'asks')
    best_bid=_column(table,'best_bid',True);best_ask=_column(table,'best_ask',True)
    new_tick=_column(table,'new_tick_size',True);hashes=_column(table,'transaction_hash')
    tokens={m['up_token']:0,m['down_token']:1}
    levels=[{'bids':{},'asks':{}},{'bids':{},'asks':{}}]
    seeded=[False,False];full_book_seen=[False,False];token_recv=[0,0];token_src=[0,0];tick_sizes=[10000,10000]
    expected=[[None,None],[None,None]];i=0
    times=np.arange(m['start_ms']-65000,m['expiry_ms']+5001,50,dtype=np.int64)
    tops=np.zeros((len(times),2,2),dtype=np.int64);usable=np.zeros(len(times),dtype=bool)
    public=[];seen=set();dupes=0;mismatched=0;event_counts=Counter(kinds.tolist())
    actual_points=[]
    # Source-clock top-of-book timeline for labels and strict pre-trade matching.
    source_top=[[0,0],[0,0]];source_updated=[0,0]
    source_idx=np.argsort(src,kind='stable')
    for j in source_idx:
        ti=tokens.get(assets[j]);kind=kinds[j]
        if ti is None:continue
        if kind=='price_change' and np.isfinite(best_bid[j]) and np.isfinite(best_ask[j]):
            source_top[ti]=[round(best_bid[j]*1e6),round(best_ask[j]*1e6)]
        elif kind=='book':
            bb=json.loads(bids[j]) if bids[j] else [];aa=json.loads(asks[j]) if asks[j] else []
            source_top[ti]=[max((round(float(a[0])*1e6) for a in bb),default=0),min((round(float(a[0])*1e6) for a in aa),default=1000000)]
        else:continue
        source_updated[ti]=int(src[j])
        if min(source_updated)>0 and all(0<v[0]<v[1]<1000000 for v in source_top) and src[j]-min(source_updated)<=1500:
            actual_points.append((int(src[j]),*source_top[0],*source_top[1]))
    # Multiple updates in a source millisecond are inherently unordered across
    # tokens; retain the final state and use strictly earlier states for trade matching.
    actual=np.asarray(actual_points,dtype=np.int64).reshape(-1,5)
    if len(actual):actual=actual[np.r_[actual[:-1,0]!=actual[1:,0],True]]
    btc=_load_btc(out,m['start_ms'],m['expiry_ms'])
    events_path=dest/'events.jsonl'
    with events_path.open('w') as f:
        for k,now in enumerate(times):
            while i<len(table) and recv[i]<=now:
                ti=tokens.get(assets[i]);kind=kinds[i]
                if ti is None:i+=1;continue
                if kind in ('book','price_change') and token_recv[ti] and recv[i]-token_recv[ti]>3_600_000:
                    levels[ti]={'bids':{},'asks':{}};seeded[ti]=False
                if kind=='book':
                    levels[ti]['bids']={round(float(p)*1e6):float(q) for p,q in (json.loads(bids[i]) if bids[i] else []) if float(q)>0}
                    levels[ti]['asks']={round(float(p)*1e6):float(q) for p,q in (json.loads(asks[i]) if asks[i] else []) if float(q)>0}
                    seeded[ti]=True;full_book_seen[ti]=True
                    expected[ti]=[max(levels[ti]['bids'],default=0),min(levels[ti]['asks'],default=1000000)]
                    token_recv[ti]=int(recv[i]);token_src[ti]=int(src[i])
                elif kind=='price_change' and np.isfinite(prices[i]) and np.isfinite(sizes[i]) and sides[i] in ('BUY','SELL'):
                    side='bids' if sides[i]=='BUY' else 'asks';price=round(prices[i]*1e6)
                    if sizes[i]>0:levels[ti][side][price]=float(sizes[i])
                    else:levels[ti][side].pop(price,None)
                    expected[ti]=[round(best_bid[i]*1e6) if np.isfinite(best_bid[i]) else None,round(best_ask[i]*1e6) if np.isfinite(best_ask[i]) else None]
                    # NBBO is direct evidence that levels beyond the reported
                    # touch no longer exist. This never advances resting queues.
                    if expected[ti][0] is not None:
                        for stale in [p for p in levels[ti]['bids'] if p>expected[ti][0]]:levels[ti]['bids'].pop(stale)
                    if expected[ti][1] is not None:
                        for stale in [p for p in levels[ti]['asks'] if p<expected[ti][1]]:levels[ti]['asks'].pop(stale)
                    seeded[ti]=True
                    token_recv[ti]=int(recv[i]);token_src[ti]=int(src[i])
                elif kind=='tick_size_change' and np.isfinite(new_tick[i]):tick_sizes[ti]=round(new_tick[i]*1e6)
                elif kind=='last_trade_price' and m['start_ms']<=src[i]<m['expiry_ms'] and np.isfinite(prices[i]) and np.isfinite(sizes[i]) and sides[i] in ('BUY','SELL'):
                    signature=(int(src[i]),ti,str(hashes[i]),str(sides[i]),float(prices[i]),float(sizes[i]))
                    if signature in seen:dupes+=1
                    else:
                        seen.add(signature)
                        qty=int(np.floor(sizes[i]))
                        if qty>0:public.append({'type':'public_trade','event_ms':int(src[i]),'recv_ms':int(recv[i]),'token':'up' if ti==0 else 'down','taker_buy':sides[i]=='BUY','price_tick':round(prices[i]*1e6),'quantity_lots':qty,'raw_quantity':float(sizes[i])})
                i+=1
            snapshots=[];valid=True
            for ti in range(2):
                bb=sorted(levels[ti]['bids'].items(),reverse=True)[:16];aa=sorted(levels[ti]['asks'].items())[:16]
                touch=[bb[0][0] if bb else 0,aa[0][0] if aa else 1000000];tops[k,ti]=touch
                matches=all(want is None or want==got for want,got in zip(expected[ti],touch))
                healthy=seeded[ti] and bool(bb) and bool(aa) and 0<touch[0]<touch[1]<1000000 and now-token_recv[ti]<=1500 and matches and tick_sizes[ti]>0 and 10000%tick_sizes[ti]==0
                if not matches:mismatched+=1
                valid&=healthy
                snapshots.append({'usable':bool(healthy),'recv_ms':token_recv[ti],'bids':bb,'asks':aa})
            usable[k]=valid
            # Both original token receive clocks remain unchanged even though
            # the strategy polls the reconstructed state at 50ms intervals.
            e={'type':'books','event_ms':min(token_src),'recv_ms':int(now),'up':snapshots[0],'down':snapshots[1]}
            f.write(json.dumps(e,separators=(',',':'))+'\n')
        for e in public:f.write(json.dumps(e,separators=(',',':'))+'\n')
        for j in range(len(btc[0])):
            e={'type':'agg_trade','event_ms':int(btc[0][j]),'recv_ms':int(btc[1][j]),'price':float(btc[2][j]),'quantity':float(btc[3][j]),'buyer_is_maker':bool(btc[4][j])}
            f.write(json.dumps(e,separators=(',',':'))+'\n')
    x,ready=build_features(times,tops,usable,btc,m['expiry_ms'])
    pm_x,pm_ready=build_features(times,tops,usable,btc,m['expiry_ms'],True)
    training=[];pm_training=[]
    at=actual[:,0] if len(actual) else np.array([],dtype=np.int64)
    ap=(actual[:,1]+actual[:,2]+2000000-actual[:,3]-actual[:,4])/4e6 if len(actual) else np.array([])
    unmatched=0;fractional_lots_lost=0.
    for e in public:
        t=e['event_ms'];ti=0 if e['token']=='up' else 1
        before=np.searchsorted(at,t,side='left')-1;after=np.searchsorted(at,t+1000,side='right')-1
        decision=np.searchsorted(times,t-100,side='right')-1
        fractional_lots_lost+=e['raw_quantity']-e['quantity_lots']
        if before<0 or after<0 or decision<0 or t-at[before]>1500 or t+1000-at[after]>1500:unmatched+=1;continue
        expected_price=actual[before,2+ti*2] if e['taker_buy'] else actual[before,1+ti*2]
        if e['price_tick']!=expected_price:unmatched+=1;continue
        group=0 if (ti==0) != e['taker_buy'] else 1
        y=float(ap[after]-x[decision,ti,10])
        if ready[decision]:training.append((group,int(t),y,*x[decision,ti]))
        if pm_ready[decision]:pm_training.append((group,int(t),y,*pm_x[decision,ti]))
    np.savez_compressed(dest/'features.npz',times=times,features=x,ready=ready,pm_features=pm_x,pm_ready=pm_ready,
        training=np.asarray(training,dtype=float).reshape(-1,16),pm_training=np.asarray(pm_training,dtype=float).reshape(-1,16))
    live=(times>=m['start_ms'])&(times<m['expiry_ms'])
    audit={'slug':m['slug'],'raw_rows':len(table),'event_counts':dict(event_counts),'negative_source_receive_lag_dropped':negative,
        'book_seeded_up':full_book_seen[0],'book_seeded_down':full_book_seen[1],'paired_usable_fraction':float(np.mean(usable[live])),
        'external_features_ready_fraction':float(np.mean(ready[live])),'pm_features_ready_fraction':float(np.mean(pm_ready[live])),
        'touch_mismatch_token_ticks':mismatched,'public_trades':len(public),'duplicate_public_events_dropped':dupes,
        'fractional_volume_floor_lost':fractional_lots_lost,'strict_trade_samples':len(training),'pm_trade_samples':len(pm_training),'unmatched_or_missing_labels':unmatched,
        'measured_pm_receive_clock':'PMXT exporter clock, not our own network receive clock','binance_receive_delay_assumption_ms':20,
        'decision_poll_ms':50,'book_export_depth':16,'winner':m['winner']}
    save(audit_path,audit);return audit


def normalize_all(out,workers,train_only=False,test_only=False):
    markets=json.loads((out/'markets.json').read_text());results=[]
    if train_only:markets=[m for m in markets if m.get('start_ms',0)<int(dt.datetime(2026,4,21,tzinfo=UTC).timestamp()*1000)]
    if test_only:markets=[m for m in markets if m.get('start_ms',0)>=int(dt.datetime(2026,4,21,tzinfo=UTC).timestamp()*1000)]
    with futures.ProcessPoolExecutor(max_workers=workers) as pool:
        jobs={pool.submit(normalize_market,(str(out),m)):m for m in markets if 'error'not in m}
        for j in futures.as_completed(jobs):
            m=jobs[j]
            try:r=j.result()
            except Exception as e:r={'slug':m['slug'],'error':str(e)}
            results.append(r)
            if len(results)%25==0:print('normalized',len(results),'/',len(jobs),'errors',sum('error'in r for r in results),flush=True)
    save(out/('market_coverage_train.json' if train_only else 'market_coverage_test.json' if test_only else 'market_coverage.json'),sorted(results,key=lambda x:x['slug']))
    if any('error'in r for r in results):raise RuntimeError('market normalization errors; inspect market_coverage.json')


def repair_expiry_tail(out,workers):
    """Keep public trades after expiry while outstanding cancellations are in flight.

    This does not change training features or labels. Commit marker prevents
    replay from reading an event file while its post-expiry tail is appended.
    """
    markets=json.loads((out/'markets.json').read_text())
    def one(m):
        dest=out/'normalized'/m['slug'];marker=dest/'expiry_tail.json'
        if marker.exists():return json.loads(marker.read_text())
        deadline=time.monotonic()+1800
        while not (dest/'coverage.json').exists():
            if time.monotonic()>deadline:raise TimeoutError('normalization not ready '+m['slug'])
            time.sleep(2)
        parts=[]
        for path in sorted((out/'market_rows'/m['slug']).glob('*.parquet')):
            t=pq.read_table(path,columns=['timestamp','timestamp_received','event_type','asset_id','side','price','size','transaction_hash'])
            source=pc.cast(t['timestamp'],pa.int64())
            mask=pc.and_(pc.and_(pc.greater_equal(source,m['expiry_ms']),pc.less(source,m['expiry_ms']+5000)),pc.equal(t['event_type'],'last_trade_price'))
            part=t.filter(mask)
            if len(part):
                part=part.set_column(part.schema.get_field_index('timestamp'),'timestamp',pc.cast(part['timestamp'],pa.int64()))
                part=part.set_column(part.schema.get_field_index('timestamp_received'),'timestamp_received',pc.cast(part['timestamp_received'],pa.int64()))
                parts.extend(part.to_pylist())
        public=[];seen=set();tokens={m['up_token']:'up',m['down_token']:'down'};invalid=0;floor_lost=0.
        for row in parts:
            source=row['timestamp'];recv=row['timestamp_received']
            price=float(row['price']) if row['price'] is not None else 0.;qty=float(row['size']) if row['size'] is not None else 0.
            token=tokens.get(row['asset_id']);side=row['side']
            signature=(source,token,str(row['transaction_hash']),side,price,qty)
            if signature in seen:continue
            seen.add(signature)
            if token is None or side not in ('BUY','SELL') or recv<source or not 0<price<1 or not np.isfinite(qty) or qty<1:
                invalid+=1;continue
            lots=int(np.floor(qty));floor_lost+=qty-lots
            public.append({'type':'public_trade','event_ms':source,'recv_ms':recv,'token':token,'taker_buy':side=='BUY',
                'price_tick':round(price*1e6),'quantity_lots':lots,'raw_quantity':qty})
        with (dest/'events.jsonl').open('a') as f:
            for e in sorted(public,key=lambda e:(e['event_ms'],e['recv_ms'])):f.write(json.dumps(e,separators=(',',':'))+'\n')
        audit={'slug':m['slug'],'tail_horizon_ms':5000,'public_trades':len(public),'invalid_or_subshare_rows':invalid,
               'fractional_volume_floor_lost':floor_lost,'training_unchanged':True}
        save(marker,audit);return audit
    results=[]
    with futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for r in pool.map(one,markets):
            results.append(r)
            if len(results)%100==0:print('expiry tails',len(results),'/',len(markets),flush=True)
    save(out/'expiry_tail_audit.json',results)


def fit_models(out):
    """Predetermined weighted Ridge; each training contract gets total weight one."""
    cutoff=int(dt.datetime(2026,4,21,tzinfo=UTC).timestamp()*1000)
    markets=[m for m in json.loads((out/'markets.json').read_text()) if m['start_ms']<cutoff]
    manifest={'cutoff_ms':cutoff,'training_contracts':len(markets),'label_horizon_ms':1000,
        'feature_lead_ms':100,'ridge_penalty':1.0,'contract_weighting':'total weight one per contract before group split',
        'hyperparameters_selected_on_test':False,'models':{}}
    for pm_only,name,key in ((False,'external','training'),(True,'pm_only','pm_training')):
        parts=[];weights=[];contracts_with_samples=0
        for m in markets:
            with np.load(out/'normalized'/m['slug']/'features.npz') as z:rows=z[key]
            rows=rows[rows[:,1]+1000<cutoff]
            if len(rows):parts.append(rows);weights.append(np.full(len(rows),1/len(rows)));contracts_with_samples+=1
        if not parts:raise ValueError('no strict training samples')
        rows=np.concatenate(parts);w=np.concatenate(weights)
        artifact={'schema':'btc15m-lead-lag-v1','symbol':'BTCUSDT','duration_ms':900000,
            'preprocessing':'recv-clock-rv1s-60s-v1','feature_names':FEATURES,'trained_until_ms':cutoff,
            'synthetic':False,'pm_only':pm_only,'external_feature_clip':0.25}
        stats={'samples':len(rows),'contracts_with_samples':contracts_with_samples,'groups':{}}
        for group,group_name in ((0,'plus'),(1,'minus')):
            select=rows[:,0]==group;x=rows[select,3:];y=rows[select,2];wg=w[select]
            if not len(y):raise ValueError('no training samples for '+group_name)
            mean=np.average(x,axis=0,weights=wg);scale=np.sqrt(np.average((x-mean)**2,axis=0,weights=wg))
            scale=np.where(scale>1e-12,scale,1.0)
            if pm_only:mean[:5]=0;scale[:5]=1
            z=(x-mean)/scale;intercept=float(np.average(y,weights=wg))
            start=5 if pm_only else 0;zs=z[:,start:]
            beta=np.linalg.solve(zs.T@(wg[:,None]*zs)+np.eye(zs.shape[1]),zs.T@(wg*(y-intercept)))
            coefficients=np.r_[np.zeros(start),beta]
            artifact[group_name]={'mean':mean.tolist(),'scale':scale.tolist(),'coefficients':coefficients.tolist(),'intercept':intercept}
            residual=y-(intercept+z@coefficients)
            stats['groups'][group_name]={'samples':len(y),'weighted_training_rmse':float(np.sqrt(np.average(residual**2,weights=wg))),
                'label_mean':intercept,'label_std':float(np.sqrt(np.average((y-intercept)**2,weights=wg)))}
        path=out/'models'/(name+'.json');save(path,artifact)
        stats['sha256']=hashlib.sha256(path.read_bytes()).hexdigest();manifest['models'][name]=stats
    save(out/'models'/'fit_manifest.json',manifest);print(json.dumps(manifest,indent=2),flush=True)


PROFILES={'external':('external',False,100,'continuous'),
          'pm_only_common':('pm_only',True,100,'continuous'),
          'external_cancel200':('external',False,200,'continuous'),
          'shock_entry':('external',False,100,'shock_entry')}


def replay_config(m,profile):
    model,common,cancel,candidate=PROFILES[profile]
    return {'strategy':'btc15m_lead_lag','candidate':candidate,'model_path':None,
        'pm_only_require_external_readiness':common,
        'market':{'market_id':m['condition_id'],'symbol':'BTCUSDT','up_token':m['up_token'],
                  'down_token':m['down_token'],'start_ms':m['start_ms'],'expiry_ms':m['expiry_ms']},
        'quote':{'grid_tick':10000,'buffer_tick':2500,'inventory_skew_tick_per_lot':250,
                 'queue_tolerance_tick':10000,'size_lots':5,'min_size_lots':5,'max_net_lots':50,
                 'max_token_lots':100,'max_data_age_ms':1500,'ttl_ms':5000},
        'regime':{'calm_tick':5000,'shock_tick':15000,'halt_tick':40000,'quiet_recovery_ms':1500},
        'latency_assumptions_ms':{'submit':25,'cancel':cancel,'fill_notice':50,'terminal_reduce':275},
        'queue_factor':1.0,'decision_interval_ms':50,'terminal_reduce_request_ms':None,
        'accounting':{'initial_cash_tick':100000000,'initial_pairs':25,'maker_fee_bps':0,
                      'taker_fee_bps':0,'operating_cost_tick':0}}


def run_backtests(out,workers,profiles):
    cutoff=int(dt.datetime(2026,4,21,tzinfo=UTC).timestamp()*1000)
    markets=[m for m in json.loads((out/'markets.json').read_text()) if m['start_ms']>=cutoff]
    executable=Path('build/run_btc15m_lead_lag_replay').resolve()
    executable_hash=hashlib.sha256(executable.read_bytes()).hexdigest()
    hashes={name:hashlib.sha256((out/'models'/(name+'.json')).read_bytes()).hexdigest() for name in ('external','pm_only')}
    jobs=[(m,p) for m in markets for p in profiles]
    def one(job):
        m,p=job;dest=out/'backtests'/p/m['slug'];dest.mkdir(parents=True,exist_ok=True)
        result_path=dest/'summary.json';config=replay_config(m,p);model=PROFILES[p][0]
        if result_path.exists():
            old=json.loads(result_path.read_text())
            if old.get('model_sha256')==hashes[model] and old.get('config')==config and old.get('executable_sha256')==executable_hash:return old
            raise ValueError('cached result differs from frozen model/config: '+str(result_path))
        save(dest/'config.json',config)
        events=out/'normalized'/m['slug']/'events.jsonl'
        coverage=events.with_name('expiry_tail.json')
        # Normalization can run alongside replay. Its final coverage file is
        # the completion marker, so partially written event files stay invisible.
        deadline=time.monotonic()+1800
        while not coverage.exists():
            if time.monotonic()>deadline:raise TimeoutError('normalization not complete: '+m['slug'])
            time.sleep(2)
        command=[str(executable),'--config',str(dest/'config.json'),'--events',str(out/'normalized'/m['slug']/'events.jsonl'),
                 '--model',str(out/'models'/(model+'.json')),'--settlement',m['winner'].lower()]
        if m==markets[0]:command+=['--trace',str(dest/'trace.jsonl')]
        run=subprocess.run(command,capture_output=True,text=True,timeout=180)
        if run.returncode:raise RuntimeError(m['slug']+' '+p+': '+run.stderr[-1000:])
        r=json.loads(run.stdout);r.update({'slug':m['slug'],'start_ms':m['start_ms'],'winner':m['winner'],
            'profile':p,'model_sha256':hashes[model],'executable_sha256':executable_hash,'config':config})
        if r['synthetic'] or not r['model_loaded']:raise ValueError('invalid model provenance')
        if r['cash_tick']!=r['known_cash_tick'] or r['up_lots']!=r['known_up_lots'] or r['down_lots']!=r['known_down_lots']:
            raise ValueError('unreconciled inventory: '+m['slug'])
        if r['final_reserved_lots']!=0:raise ValueError('outstanding reservation: '+m['slug'])
        save(result_path,r);return r
    results=[]
    with futures.ThreadPoolExecutor(max_workers=workers) as pool:
        pending={pool.submit(one,j):j for j in jobs}
        for task in futures.as_completed(pending):
            m,p=pending[task]
            try:r=task.result()
            except Exception as e:r={'slug':m['slug'],'profile':p,'error':str(e)}
            results.append(r)
            if len(results)%50==0:print('replayed',len(results),'/',len(jobs),'errors',sum('error'in r for r in results),flush=True)
    save(out/'backtests'/('batch_'+'_'.join(profiles)+'.json'),results)
    if any('error'in r for r in results):raise RuntimeError('backtest errors; inspect batch results')


def aggregate_report(out):
    all_markets=json.loads((out/'markets.json').read_text())
    coverage=[json.loads(p.read_text()) for p in sorted((out/'normalized').glob('*/coverage.json'))]
    if len(coverage)!=len(all_markets):raise ValueError('normalization coverage is incomplete')
    extraction=json.loads((out/'extraction_audit.json').read_text())
    totals={'requested_hours':336,'requested_markets':len(all_markets),'normalized_markets':len(coverage),
        'extraction_rows':sum(r.get('selected_rows',0) for r in extraction),
        'extraction_downloaded_bytes':sum(r.get('downloaded_bytes',0) for r in extraction),
        'training_contracts':384,'test_contracts':960,'profiles':{},'paired_comparisons':{}}
    by_profile={}
    for profile in PROFILES:
        rows=[json.loads(p.read_text()) for p in sorted((out/'backtests'/profile).glob('*/summary.json'))]
        if not rows:continue
        if len(rows)!=960:raise ValueError('incomplete test profile '+profile)
        by_profile[profile]={r['slug']:r for r in rows}
        pnl=np.array([r['settlement_pnl_tick']/1e6 for r in rows])
        quantities=np.sum([r['markout_quantity'] for r in rows],axis=0)
        marks=[sum((r['quantity_weighted_markout'][i] or 0)*r['markout_quantity'][i] for r in rows)/int(quantities[i]) if quantities[i] else None for i in range(3)]
        daily={}
        for r in rows:
            day=dt.datetime.fromtimestamp(r['start_ms']/1000,UTC).strftime('%Y-%m-%d')
            daily[day]=daily.get(day,0)+r['settlement_pnl_tick']/1e6
        totals['profiles'][profile]={'contracts':len(rows),'pnl_usd':float(pnl.sum()),'median_contract_pnl_usd':float(np.median(pnl)),
            'profitable_contracts':int(np.sum(pnl>0)),'losing_contracts':int(np.sum(pnl<0)),'zero_contracts':int(np.sum(pnl==0)),
            'no_quotes_contracts':sum(r['submitted']==0 for r in rows),'no_fills_contracts':sum(r['fills']==0 for r in rows),
            'submitted':sum(r['submitted'] for r in rows),'risk_rejected':sum(r['risk_rejected'] for r in rows),
            'fills':sum(r['fills'] for r in rows),'cancel_requests':sum(r['cancel_requests'] for r in rows),
            'cancel_pending_fills':sum(r['cancel_pending_fills'] for r in rows),'cancel_pending_fill_lots':sum(r['cancel_pending_fill_lots'] for r in rows),
            'post_expiry_fills':sum(r['post_expiry_fills'] for r in rows),'post_expiry_fill_lots':sum(r['post_expiry_fill_lots'] for r in rows),
            'weighted_markout_100ms_1s_5s':marks,'markout_quantity':quantities.tolist(),
            'end_abs_net_lots_mean':float(np.mean([abs(r['up_lots']-r['down_lots']) for r in rows])),
            'end_abs_net_lots_max':max(abs(r['up_lots']-r['down_lots']) for r in rows),
            'cash_change_usd':sum((r['cash_tick']-100000000)/1e6 for r in rows),
            'settlement_inventory_change_usd':sum((r['up_lots'] if r['winner']=='Up' else r['down_lots'])-25 for r in rows),
            'daily_pnl_usd':daily,'five_best_contracts_pnl_usd':float(np.sort(pnl)[-5:].sum())}
        totals['profiles'][profile]['profitable_days']=sum(v>0 for v in daily.values())
        values=np.array(list(daily.values()))
        rng=np.random.default_rng(20260421)
        sampled=values[rng.integers(0,len(values),size=(10000,len(values)))].sum(axis=1)
        totals['profiles'][profile]['day_block_bootstrap_total_pnl_95pct_usd']=np.quantile(sampled,[.025,.975]).tolist()
    if 'external'in by_profile:
        for name in ('pm_only_common','external_cancel200','shock_entry'):
            if name not in by_profile:continue
            common=sorted(by_profile['external'].keys()&by_profile[name].keys())
            differences=np.array([(by_profile['external'][s]['settlement_pnl_tick']-by_profile[name][s]['settlement_pnl_tick'])/1e6 for s in common])
            totals['paired_comparisons']['external_minus_'+name]={'contracts':len(common),'pnl_difference_usd':float(differences.sum()),
                'median_difference_usd':float(np.median(differences)),'external_better_contracts':int(np.sum(differences>0))}
    for key in ('paired_usable_fraction','external_features_ready_fraction','pm_features_ready_fraction'):
        values=np.array([r[key] for r in coverage]);totals[key]={'mean':float(np.mean(values)),'min':float(np.min(values)),'p10':float(np.quantile(values,.1))}
    test_slugs={m['slug'] for m in all_markets if m['start_ms']>=int(dt.datetime(2026,4,21,tzinfo=UTC).timestamp()*1000)}
    test_coverage=[r for r in coverage if r['slug'] in test_slugs]
    totals['test_data_coverage']={key:float(np.mean([r[key] for r in test_coverage])) for key in ('paired_usable_fraction','external_features_ready_fraction','pm_features_ready_fraction')}
    tails=json.loads((out/'expiry_tail_audit.json').read_text())
    totals['post_expiry_public_trades']=sum(r['public_trades'] for r in tails)
    totals['negative_source_receive_lag_dropped']=sum(r['negative_source_receive_lag_dropped'] for r in coverage)
    totals['fractional_volume_floor_lost']=sum(r['fractional_volume_floor_lost'] for r in coverage)
    save(out/'results.json',totals);print(json.dumps(totals,indent=2),flush=True)
    lines=['# PMXT BTC15m 真实历史数据回测',
        '', 'UTC 2026-04-17 至 2026-04-30。336/336 请求小时可下载；1344 个市场。',
        '前 384 个市场训练、冻结新模型；后 960 个市场样本外测试。结果为公共成交队列模型下的模拟 PnL。',
        '', '| 配置 | 样本外市场 | 结算 PnL ($) | 盈利天数 / 10 | 成交次数 | 撤单等待期间成交股数 |',
        '|---|---:|---:|---:|---:|---:|']
    for name,r in totals['profiles'].items():
        lines.append(f"| {name} | {r['contracts']} | {r['pnl_usd']:.2f} | {r['profitable_days']} | {r['fills']} | {r['cancel_pending_fill_lots']} |")
    lines+=['','| 配置 | 100ms markout (每股美分) | 1s | 5s | 期末平均绝对净库存 |', '|---|---:|---:|---:|---:|']
    for name,r in totals['profiles'].items():
        marks=['缺失' if v is None else f'{v*100:.4f}' for v in r['weighted_markout_100ms_1s_5s']]
        lines.append(f"| {name} | {' | '.join(marks)} | {r['end_abs_net_lots_mean']:.3f} |")
    lines+=['','| 配置 | 现金变化 ($) | 获胜库存相对初始的结算变化 ($) | 到期后成交股数 |', '|---|---:|---:|---:|']
    for name,r in totals['profiles'].items():
        lines.append(f"| {name} | {r['cash_change_usd']:.2f} | {r['settlement_inventory_change_usd']} | {r['post_expiry_fill_lots']} |")
    lines+=['','每日模拟 PnL：','','| UTC 日期 | '+' | '.join(totals['profiles'])+' |', '|---|'+'---:|'*len(totals['profiles'])]
    for day in sorted(next(iter(totals['profiles'].values()))['daily_pnl_usd']):
        lines.append('| '+day+' | '+' | '.join(f"{r['daily_pnl_usd'][day]:.2f}" for r in totals['profiles'].values())+' |')
    lines+=['',f"实际提取 {totals['extraction_rows']:,} 条记录，读取约 {totals['extraction_downloaded_bytes']/1e9:.2f} GB 字节范围。",
        f"样本外成对盘口可用率 {totals['test_data_coverage']['paired_usable_fraction']:.2%}；外部特征就绪率 {totals['test_data_coverage']['external_features_ready_fraction']:.2%}。",
        '', '各市场独立启动：25 对库存购入后现金 $100，初始资本 $125；不是复利资金曲线。',
        'PM-only 独立训练并控制外部数据可用性；所有 960 个测试市场均计入。',
        'PM 接收时间是 PMXT exporter 时钟。BTC 接收 20ms、发单 25ms、撤单 100/200ms、成交通知 50ms 均为假设。',
        'TTL 与到期撤单均需等待撤单延迟；真实到期后公共成交继续检验尚未撤掉的订单。',
        'Maker 手续费设置为零；rebate、运营成本为零；未启用末端 Taker 减仓。残留库存按实际结果结算。',
        '盘口只有已知价位，队列仅由公共成交推进，未还原 FIFO、提交时完整盘口、自身冲击或实际交易所订单确认。',
        '这是十天样本外的模拟结果，不能证明实盘可成交性或稳定盈利。',
        '', '机器可读汇总：[results.json](results.json)；逐市场结果：[backtests](backtests/)；',
        '冻结模型：[models](models/)；逐小时覆盖：[coverage_summary.json](coverage_summary.json)；',
        '逐市场覆盖：[market_coverage_test.json](market_coverage_test.json)；运行来源与假设：[run_manifest.json](run_manifest.json)。',
        '原始对象：[PMXT archive](https://archive.pmxt.dev/Polymarket/v2)；外部数据：[Binance public data](https://github.com/binance/binance-public-data)。',
        '复现方法及模型接口：[项目说明](../../docs/pmxt_btc15m_backtest.md)。','']
    (out/'README.md').write_text('\n'.join(lines))


def main():
    p=argparse.ArgumentParser();p.add_argument('phase',choices=['audit','fetch-pmxt','fetch-binance','shard','normalize','repair-tail','fit','backtest','report']);p.add_argument('--out',type=Path,default=Path('runs/pmxt_btc15m_20260417_20260430'));p.add_argument('--workers',type=int,default=6);p.add_argument('--train-only',action='store_true');p.add_argument('--test-only',action='store_true');p.add_argument('--profiles',nargs='+',choices=list(PROFILES),default=['external','pm_only_common']);a=p.parse_args()
    if a.phase=='audit':audit_inputs(a.out,a.workers)
    elif a.phase=='fetch-pmxt':fetch_hours(a.out,a.workers)
    elif a.phase=='fetch-binance':fetch_binance(a.out,a.workers)
    elif a.phase=='shard':shard_hours(a.out)
    elif a.phase=='normalize':normalize_all(a.out,a.workers,a.train_only,a.test_only)
    elif a.phase=='repair-tail':repair_expiry_tail(a.out,a.workers)
    elif a.phase=='fit':fit_models(a.out)
    elif a.phase=='backtest':run_backtests(a.out,a.workers,a.profiles)
    elif a.phase=='report':aggregate_report(a.out)
if __name__=='__main__':main()

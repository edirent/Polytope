#!/usr/bin/env python3
"""Post-freeze accounting and regime diagnostics; never change trading inputs."""
from __future__ import annotations
import argparse
import ast
import concurrent.futures as futures
from collections import deque
import datetime as dt
import gzip
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import pmxt_btc15m_backtest as base
import pmxt_shock_fixed_tests as suite

OUT=suite.OUT/'diagnostics'
VERSION=1
METRICS=['abs_alpha','btc_sigma_bps_per_sqrt_second','tau_seconds',
         'abs_distance_proxy_bps','abs_distance_proxy_sigma','spread','anchor_distance_from_half']
HORIZONS=[250,1000,5000,10000]
REGIMES={'CALM':0,'SHIFT':1,'SHOCK':2,'HALT':3,'WAIT':4}


def day(ms):return dt.datetime.fromtimestamp(ms/1000,base.UTC).date().isoformat()


def decompose(fills,winner,method='fifo'):
    """Match equivalent UP exposure, with zero starting directional exposure.

    C_eff=C+DOWN; N=UP-DOWN. An initial pair costs/settles exactly one.
    BuyUp/SellDown are long UP at p/(1-p); SellUp/BuyDown are short
    UP at p/(1-p). Opposite exposure closes without using the outcome.
    All unclosed exposure settles against its own remaining cost basis.
    """
    lots=deque();closed=0;categories={'same_token':0,'buy_pair':0,'sell_pair':0}
    matched=0;net=0;cash=0;token_delta=[0,0]
    for f in fills:
        c=f['channel'];q=f['quantity_lots'];p=f['price_tick'];ti=c//2
        if f['terminal']:raise ValueError('terminal taker unexpectedly enabled')
        sign=1 if c in (0,3) else -1;equivalent=p if ti==0 else 1_000_000-p
        bid=c in (0,2);cash+=(-1 if bid else 1)*q*p
        token_delta[ti]+=(1 if bid else -1)*q;net+=sign*q
        left=q
        while left and lots and lots[0]['sign']!=sign:
            old=lots[-1] if method=='lifo' else lots[0];take=min(left,old['qty'])
            # Old long: sell price minus buy price; old short: reverse.
            value=take*old['sign']*(equivalent-old['price'])
            closed+=value;matched+=take
            category='same_token' if old['channel']//2==ti else ('buy_pair' if bid else 'sell_pair')
            categories[category]+=value
            left-=take;old['qty']-=take
            if not old['qty']:
                if method=='lifo':lots.pop()
                else:lots.popleft()
        if left:lots.append({'sign':sign,'qty':left,'price':equivalent,'channel':c})
    y=1_000_000 if winner=='Up' else 0
    terminal=sum(x['sign']*x['qty']*(y-x['price']) for x in lots)
    total=cash+(token_delta[0] if winner=='Up' else token_delta[1])*1_000_000
    if closed+terminal!=total or sum(x['sign']*x['qty'] for x in lots)!=net:
        raise ValueError('exposure decomposition failed')
    return {'closed_paired_tick':closed,'terminal_directional_tick':terminal,'total_tick':total,
            'categories_tick':categories,'matched_exposure_lots':matched,'terminal_net_lots':net,
            'fill_cash_change_tick':cash,'token_delta':token_delta,
            'residual_equivalent_up_cost_tick':sum(x['qty']*x['price'] for x in lots)}


def average_decompose(fills,winner):
    net=0;cost=0.;closed=0.
    for f in fills:
        c=f['channel'];sign=1 if c in (0,3) else -1;q=f['quantity_lots']
        p=f['price_tick'] if c//2==0 else 1_000_000-f['price_tick']
        if net and np.sign(net)!=sign:
            take=min(abs(net),q);mean=cost/abs(net)
            closed+=take*np.sign(net)*(p-mean);cost-=take*mean;net+=sign*take;q-=take
        if q:cost+=q*p;net+=sign*q
    y=1_000_000 if winner=='Up' else 0
    terminal=net*y-np.sign(net)*cost
    return {'closed_paired_tick':closed,'terminal_directional_tick':float(terminal)}


def btc_state(events,times,start,delay):
    trades=[e for e in events if e['type']=='agg_trade']
    source=np.array([e['event_ms'] for e in trades],dtype=np.int64)
    price=np.array([e['price'] for e in trades]);recv=source+delay
    ix=np.argsort(recv,kind='stable');source=source[ix];recv=recv[ix];price=price[ix]
    # Match engine's policy: an older source price never overwrites a newer one.
    keep=source>=np.maximum.accumulate(source);source=source[keep];recv=recv[keep];price=price[keep]
    logp=np.log(price)
    def lag(t):
        j=np.searchsorted(recv,t,side='right')-1;safe=np.maximum(j,0)
        return logp[safe],(j>=0)&(t-recv[safe]<=1500)
    ret=[];valid=[]
    for i in range(60):
        a,va=lag(times-i*1000);b,vb=lag(times-(i+1)*1000);ret.append(a-b);valid.append(va&vb)
    r=np.asarray(ret).T;v=np.cumprod(np.asarray(valid).T,axis=1).astype(bool)
    n=v.sum(axis=1);s=(r*v).sum(axis=1);ss=(r*r*v).sum(axis=1)
    variance=np.divide(ss-np.divide(s*s,n,out=np.zeros_like(s),where=n>0),n-1,out=np.full_like(s,np.nan),where=n>1)
    sigma=np.sqrt(np.maximum(variance,0));sigma[(n<10)|(variance<=1e-16)]=np.nan
    current,fresh=lag(times);current[~fresh]=np.nan
    k=np.searchsorted(source,start,side='right')-1
    if k<0 or start-source[k]>1500:proxy=np.nan
    else:proxy=logp[k]
    return current,sigma,(current-proxy)*10000,{'strike_proxy_price':float(np.exp(proxy)) if np.isfinite(proxy) else None,
        'strike_proxy_source_ms':int(source[k]) if np.isfinite(proxy) else None}


def visible_anchor(events,times):
    books=sorted((e for e in events if e['type']=='books'),key=lambda e:e['recv_ms'])
    bt=[];a=[];valid=[];latest_source=0
    for e in books:
        if e['event_ms']<latest_source:continue
        latest_source=e['event_ms'];u=e['up'];d=e['down'];now=e['recv_ms']
        healthy=all(t['usable'] and t['bids'] and t['asks'] and now-t['recv_ms']<=1500 for t in (u,d))
        bt.append(now);valid.append(healthy)
        a.append(((u['bids'][0][0]+u['asks'][0][0])+(2_000_000-d['bids'][0][0]-d['asks'][0][0]))/4e6 if healthy else np.nan)
    bt=np.asarray(bt);a=np.asarray(a);j=np.searchsorted(bt,times,side='right')-1;safe=np.maximum(j,0)
    value=a[safe];value[(j<0)|(times-bt[safe]>1500)]=np.nan
    return value


def episodes(times,ready,alpha,anchor,btc,regime):
    """Threshold crossings with observed predecessor; 5s declustering, 10s tail."""
    raw=ready&(np.abs(alpha)>=.015)
    onset=np.flatnonzero(raw[1:]&ready[:-1]&~raw[:-1])+1
    selected=[];last=-100000
    for i in onset:
        if i-last>=100 and i+200<len(times):selected.append(i);last=i
    records=[]
    for i in selected:
        sign=1 if alpha[i]>0 else -1
        end=i+1
        while end<len(times) and raw[end]:end+=1
        rec={'now_ms':int(times[i]),'alpha':float(alpha[i]),'eligible_shock':bool(regime[i]==2),
             'observed_duration_ms':int((end-i)*50),'ended_by_missing_state':bool(end<len(times) and not ready[end]),'horizons':{}}
        for h in HORIZONS:
            k=i+h//50
            rec['horizons'][str(h)]={'alpha_same_sign':bool(sign*alpha[k]>0) if ready[k] else None,
                'alpha_signed':float(sign*alpha[k]) if ready[k] else None,
                'alpha_below_calm':bool(abs(alpha[k])<.005) if ready[k] else None,
                'pm_signed_change':float(sign*(anchor[k]-anchor[i])) if np.isfinite(anchor[k]) and np.isfinite(anchor[i]) else None,
                'btc_signed_return_bps':float(sign*(btc[k]-btc[i])*10000) if np.isfinite(btc[k]) and np.isfinite(btc[i]) else None}
        records.append(rec)
    return records,{'raw_crossings':len(onset),'declustered_events':len(records),'threshold_occupancy_grid_points':int(raw.sum())}


def one(job):
    window,profile,slug=job;dest=OUT/window/profile/slug;result=dest/'case.json'
    if result.exists():
        r=json.loads(result.read_text())
        if r['version']!=VERSION:raise ValueError('incompatible diagnostic cache')
        return r
    original=suite.OUT/window/profile/slug;summary=json.loads((original/'summary.json').read_text())
    data=suite.OLD if window=='reused' else suite.NEW
    cfg=summary['config'];start=summary['start_ms'];expiry=cfg['market']['expiry_ms']
    dest.mkdir(parents=True,exist_ok=True)
    exe=Path('build/run_btc15m_lead_lag_replay').resolve()
    with tempfile.TemporaryDirectory(prefix='pmxt-diag-',dir=OUT/'temporary') as td:
        trace=Path(td)/'trace.jsonl'
        run=subprocess.run([str(exe),'--config',str(original/'config.json'),
            '--events',str(data/'normalized'/slug/'events.jsonl'),'--model',str(suite.OUT/'frozen_model.json'),
            '--settlement',summary['winner'].lower(),'--trace',str(trace)],capture_output=True,text=True,timeout=300)
        if run.returncode:raise RuntimeError(run.stderr[-1000:])
        replayed=json.loads(run.stdout)
        if any(summary[k]!=v for k,v in replayed.items()):raise ValueError('replayed result changed '+slug)
        fills=[];times=np.arange(start,expiry,50);n=len(times)
        ready=np.zeros(n,dtype=bool);alpha=np.full(n,np.nan);spread=np.full(n,np.nan)
        anchor=np.full(n,np.nan);regime=np.full(n,3,dtype=np.int8)
        with trace.open() as f:
            for line in f:
                if line.startswith('{"type":"fill"'):fills.append(json.loads(line));continue
                if profile!='baseline':continue
                d=json.loads(line);t=d['now_ms']
                if not start<=t<expiry or (t-start)%50:continue
                i=(t-start)//50;ready[i]=d['ready'];regime[i]=REGIMES[d['regime']]
                if d['ready']:
                    alpha[i]=d['alpha'];anchor[i]=d['anchor']
                    spread[i]=(d['features'][0][9]+d['features'][2][9])/2
    fifo=decompose(fills,summary['winner']);lifo=decompose(fills,summary['winner'],'lifo');average=average_decompose(fills,summary['winner'])
    if fifo['total_tick']!=summary['settlement_pnl_tick'] or len(fills)!=summary['fills']:
        raise ValueError('fill PnL reconciliation failed')
    if fifo['fill_cash_change_tick']!=summary['cash_tick']-cfg['accounting']['initial_cash_tick']:
        raise ValueError('fill cash reconciliation failed')
    for ti,key in enumerate(['up_lots','down_lots']):
        if cfg['accounting']['initial_pairs']+fifo['token_delta'][ti]!=summary[key]:raise ValueError('fill token reconciliation failed')
    quantities=[];marks=[]
    for j in range(3):
        q=sum(f['quantity_lots'] for f in fills if f['markout'][j] is not None)
        value=sum(f['quantity_lots']*f['markout'][j] for f in fills if f['markout'][j] is not None)/q if q else None
        if q!=summary['markout_quantity'][j] or (q and abs(value-summary['quantity_weighted_markout'][j])>1e-12):
            raise ValueError('fill markout reconciliation failed')
        quantities.append(q);marks.append(value)
    with gzip.open(dest/'fills.jsonl.gz','wt') as f:
        for fill in fills:f.write(json.dumps(fill,separators=(',',':'))+'\n')
    result_row={'version':VERSION,'window':window,'profile':profile,'slug':slug,'start_ms':start,'day_utc':day(start),
        'winner':summary['winner'],'fills':len(fills),'pnl_usd':fifo['total_tick']/1e6,'fifo':fifo,'lifo':lifo,'average_cost':average,
        'markout_quantity':quantities,'weighted_markout':marks,'replay_summary_exact_match':True,
        'source_summary_sha256':suite.sha(original/'summary.json')}
    if profile=='baseline':
        with (data/'normalized'/slug/'events.jsonl').open() as f:events=[json.loads(line) for line in f]
        btc,sigma,distance,proxy=btc_state(events,times,start,cfg['btc_receive_delay_ms'])
        visible=visible_anchor(events,times)
        if np.any(ready & (~np.isfinite(visible) | (np.abs(visible-anchor)>1e-12))):
            raise ValueError('visible anchor differs from C++ state')
        tau=(expiry-times)/1000
        distance_sigma=np.abs(distance)/10000/np.maximum(sigma*np.sqrt(tau),1e-20)
        values=np.column_stack([np.abs(alpha),sigma*10000,tau,np.abs(distance),distance_sigma,spread,np.abs(anchor-.5)])
        np.savez_compressed(dest/'state.npz',times=times,ready=ready,alpha=alpha,values=values,regime=regime)
        ev,audit=episodes(times,ready,alpha,visible,btc,regime)
        base.save(dest/'episodes.json',ev)
        result_row.update({'state_ready_points':int(ready.sum()),'total_grid_points':n,'episodes':audit,'strike_proxy':proxy})
    base.save(result,result_row);return result_row


def jobs():
    for window,profiles in [('reused',['baseline']),('confirmation',suite.CONFIRM_PROFILES)]:
        for profile in profiles:
            for f in sorted((suite.OUT/window/profile).glob('*/summary.json')):yield window,profile,f.parent.name


def analyze(workers):
    manifest=json.loads((suite.OUT/'frozen_manifest.json').read_text())
    for file,key in [(Path('build/run_btc15m_lead_lag_replay'),'executable_sha256'),(suite.OUT/'frozen_model.json','model_sha256')]:
        if suite.sha(file)!=manifest[key]:raise ValueError('frozen artifact changed')
    if manifest['strategy_config']['accounting']['maker_fee_bps'] or manifest['strategy_config']['accounting']['operating_cost_tick']:
        raise ValueError('decomposition currently assumes frozen zero fees/costs')
    OUT.mkdir(parents=True,exist_ok=True);(OUT/'temporary').mkdir(exist_ok=True)
    snapshot=OUT/'analysis_source.py'
    if not snapshot.exists():shutil.copyfile(__file__,snapshot)
    # Reporting fixes may change the live file without changing any data
    # calculation. Preserve and compare the originally executed calculations.
    def calculations(path):
        tree=ast.parse(path.read_text());names={'decompose','average_decompose','btc_state','visible_anchor','episodes','one','jobs'}
        return {node.name:ast.dump(node,include_attributes=False) for node in tree.body
                if isinstance(node,ast.FunctionDef) and node.name in names}
    if calculations(Path(__file__))!=calculations(snapshot):raise ValueError('analysis calculations changed')
    declaration={'version':VERSION,'frozen_manifest_sha256':suite.sha(suite.OUT/'frozen_manifest.json'),
        'model_sha256':manifest['model_sha256'],'executable_sha256':manifest['executable_sha256'],
        'strategy_modified':False,'expected_cases':2976,'state_grid_ms':50,'episode_decluster_ms':5000,
        'episode_tail_ms':10000,'episode_threshold':.015,'episode_horizons_ms':HORIZONS,
        'strike_proxy':'Binance last source price <= market start, age <=1500ms; not official Chainlink strike',
        'primary_decomposition':'FIFO matching equivalent UP exposure; sensitivity: LIFO and weighted average',
        'analysis_source_sha256':suite.sha(snapshot)}
    old=OUT/'manifest.json'
    if old.exists() and json.loads(old.read_text())!=declaration:raise ValueError('diagnostic declaration changed')
    base.save(old,declaration)
    work=list(jobs());done=0
    with futures.ProcessPoolExecutor(max_workers=workers) as pool:
        pending={pool.submit(one,j):j for j in work}
        for task in futures.as_completed(pending):
            task.result();done+=1
            if done%50==0:print('diagnostics',done,'/',len(work),flush=True)
    base.save(OUT/'completion.json',{'cases':done,'expected':len(work),'exact_replay_matches':done})


def pnl_summary(rows):
    result={'contracts':len(rows),'pnl_usd':sum(r['pnl_usd'] for r in rows)}
    for method in ['fifo','lifo','average_cost']:
        result[method]={key.replace('_tick','_usd'):sum(r[method][key] for r in rows)/1e6
                       for key in ['closed_paired_tick','terminal_directional_tick']}
    result['fifo_categories_usd']={key:sum(r['fifo']['categories_tick'][key] for r in rows)/1e6
                                   for key in ['same_token','buy_pair','sell_pair']}
    q=sum(r['markout_quantity'][1] for r in rows)
    result['markout_1s_quantity']=q
    result['markout_1s_cents']=sum((r['weighted_markout'][1] or 0)*r['markout_quantity'][1] for r in rows)*100/q if q else None
    result['terminal_net_lots_gross']=sum(abs(r['fifo']['terminal_net_lots']) for r in rows)
    result['closed_positive_terminal_negative_contracts']=sum(r['fifo']['closed_paired_tick']>0 and r['fifo']['terminal_directional_tick']<0 for r in rows)
    result['closed_negative_terminal_positive_contracts']=sum(r['fifo']['closed_paired_tick']<0 and r['fifo']['terminal_directional_tick']>0 for r in rows)
    return result


def describe(a):
    a=np.asarray(a,dtype=float);a=a[np.isfinite(a)]
    if not len(a):return {'n':0}
    return {'n':len(a),'mean':float(a.mean()),'p10_p50_p90_p99':np.quantile(a,[.1,.5,.9,.99]).tolist()}


def episode_summary(records):
    result={'events':len(records),'eligible_shock_events':sum(r['eligible_shock'] for r in records),
            'duration_ms':describe([r['observed_duration_ms'] for r in records]),'horizons':{}}
    for h in HORIZONS:
        result['horizons'][str(h)]={}
        for k in ['alpha_same_sign','alpha_signed','alpha_below_calm','pm_signed_change','btc_signed_return_bps']:
            a=[r['horizons'][str(h)][k] for r in records if r['horizons'][str(h)][k] is not None]
            result['horizons'][str(h)][k]=describe(a)
            if k in ('pm_signed_change','btc_signed_return_bps'):
                result['horizons'][str(h)][k]['positive_fraction']=float(np.mean(np.asarray(a)>0)) if a else None
    return result


def report():
    result={'pnl':{},'state':{},'episode_comparison':{},'methodology':json.loads((OUT/'manifest.json').read_text()),
            'report_source_sha256':suite.sha(Path(__file__))}
    for window,profiles,expected in [('reused',['baseline'],960),('confirmation',suite.CONFIRM_PROFILES,672)]:
        result['pnl'][window]={}
        for p in profiles:
            rows=[json.loads(f.read_text()) for f in sorted((OUT/window/p).glob('*/case.json'))]
            if len(rows)!=expected:raise ValueError('incomplete accounting '+window+'/'+p)
            result['pnl'][window][p]=pnl_summary(rows)
            result['pnl'][window][p]['daily']={d:pnl_summary([r for r in rows if r['day_utc']==d]) for d in sorted({r['day_utc'] for r in rows})}
        all_values=[];all_regime=[];all_alpha=[];total=0;ready_count=0;records=[];daily={};raw=0
        for path in sorted((OUT/window/'baseline').glob('*/state.npz')):
            with np.load(path) as z:
                ready=z['ready'];v=z['values'][ready];g=z['regime'][ready];a=z['alpha'][ready]
                all_values.append(v);all_regime.append(g);all_alpha.append(a);total+=len(ready);ready_count+=int(ready.sum())
            case=json.loads(path.with_name('case.json').read_text());raw+=case['episodes']['raw_crossings']
            ev=json.loads(path.with_name('episodes.json').read_text());records+=ev;d=case['day_utc']
            if d not in daily:daily[d]={'values':[],'regimes':[],'events':[],'ready':0,'total':0}
            daily[d]['values'].append(v);daily[d]['regimes'].append(g);daily[d]['events']+=ev
            daily[d]['ready']+=int(ready.sum());daily[d]['total']+=len(ready)
        values=np.concatenate(all_values);regimes=np.concatenate(all_regime);alphas=np.concatenate(all_alpha)
        def state_stats(v,g):
            return {'metrics':{k:describe(v[:,i]) for i,k in enumerate(METRICS)},
                'ready_regime_fraction':{k:float(np.mean(g==code)) for k,code in REGIMES.items()},
                'abs_alpha_ge_shock_fraction':float(np.mean(v[:,0]>=.015)),
                'abs_alpha_ge_halt_fraction':float(np.mean(v[:,0]>=.04))}
        r=state_stats(values,regimes);r.update({'total_grid_points':total,'ready_grid_points':ready_count,
            'ready_fraction':ready_count/total,'raw_shock_crossings':raw,'declustered_shocks_per_contract':len(records)/expected,
            'positive_alpha_fraction':float(np.mean(alphas>0)),'episodes':episode_summary(records),
            'eligible_shock_episodes':episode_summary([r for r in records if r['eligible_shock']]),'daily':{}})
        for d,item in sorted(daily.items()):
            v=np.concatenate(item['values']);g=np.concatenate(item['regimes'])
            r['daily'][d]=state_stats(v,g);r['daily'][d].update({'ready_fraction':item['ready']/item['total'],
                'episodes':episode_summary(item['events'])})
        # Expiry strata diagnose simple mix changes. No parameter fitting.
        r['tau_strata']={}
        for lo,hi in [(0,60),(60,300),(300,600),(600,900)]:
            select=(values[:,2]>lo)&(values[:,2]<=hi)
            r['tau_strata'][f'{lo}-{hi}']=state_stats(values[select],regimes[select])
        result['state'][window]=r
        # Keep only aggregated state in the final report; per-market grid is NPZ.
        del values,regimes,alphas,all_values,all_regime,all_alpha,daily
    for h in HORIZONS:
        old=result['state']['reused']['episodes']['horizons'][str(h)]
        new=result['state']['confirmation']['episodes']['horizons'][str(h)]
        result['episode_comparison'][str(h)]={}
        for k in ['alpha_same_sign','alpha_signed','alpha_below_calm','pm_signed_change','btc_signed_return_bps']:
            result['episode_comparison'][str(h)][k]={'reused':old[k],'confirmation':new[k]}
    base.save(OUT/'results.json',result)
    lines=['# 冻结 shock 策略：收益归因与窗口状态诊断','',
        '所有日期为 UTC；冻结模型、执行程序、策略参数均沿用原实验。逐市场重放输出与原 summary 完全相同。','',
        '## 新窗口逐日 PnL 与 1 秒 markout','',
        'markout 按有效成交股数加权，单位为美分/股，不按市场或日简单平均。','',
        '| 日期 | 基准 PnL $ | 基准 1s mo ¢ | cancel200 PnL $ | cancel200 1s mo ¢ | joint PnL $ | joint 1s mo ¢ |',
        '|---|---:|---:|---:|---:|---:|---:|']
    for d in result['pnl']['confirmation']['baseline']['daily']:
        cells=[]
        for p in suite.CONFIRM_PROFILES:
            r=result['pnl']['confirmation'][p]['daily'][d];cells += [f"{r['pnl_usd']:.2f}",f"{r['markout_1s_cents']:.4f}"]
        lines.append('| '+d+' | '+' | '.join(cells)+' |')
    lines+=['','## 可对账的 closed/paired 与期末方向库存拆分','',
        '初始 25 对库存视为 $25 的确定配对资产。令有效现金 C*=现金+DOWN，方向仓位 N=UP−DOWN；初始 N=0。',
        'Buy UP / Sell DOWN 分别等价于以 p / (1−p) 买 UP；Sell UP / Buy DOWN 分别等价于以 p / (1−p) 卖 UP。',
        '按时间 FIFO 配对相反方向成交，已闭合收益为成交价差；剩余暴露按自身未闭合成本与最终赢家结算。',
        '该配对包括同 token 往返、买入互补对、卖出互补对，完全不使用结算结果选择哪些成交闭合。',
        'closed/paired 可包含持仓期间方向变化带来的已实现价差，不能全部称作 spread capture。',
        '分项依赖成本配对方法，因此同时报告 LIFO 与移动平均成本；每种方法的总收益相同。','',
        '| 窗口 / 配置 | 总 PnL $ | FIFO closed/paired $ | FIFO terminal $ | LIFO closed $ | LIFO terminal $ | 平均成本 closed $ | 平均成本 terminal $ |',
        '|---|---:|---:|---:|---:|---:|---:|---:|']
    for w,profiles in result['pnl'].items():
        for p,r in profiles.items():
            cells=[f"{r['pnl_usd']:.2f}"]
            for m in ['fifo','lifo','average_cost']:cells += [f"{r[m]['closed_paired_usd']:.2f}",f"{r[m]['terminal_directional_usd']:.2f}"]
            lines.append('| '+w+'/'+p+' | '+' | '.join(cells)+' |')
    lines+=['','| 日期 / 基准 | closed/paired $ | terminal $ | 总收益 $ |','|---|---:|---:|---:|']
    for d,r in result['pnl']['confirmation']['baseline']['daily'].items():
        lines.append(f"| {d} | {r['fifo']['closed_paired_usd']:.2f} | {r['fifo']['terminal_directional_usd']:.2f} | {r['pnl_usd']:.2f} |")
    lines+=['','## 状态分布（两窗口 baseline；50ms 固定网格、仅 ready 状态）','',
        '固定网格排除公共成交通知额外触发决策带来的采样权重变化。缺失/非 ready 不填零；同时报告 ready 覆盖率。',
        'BTC volatility 为策略使用的过去最多 60 个、不重叠 1 秒 log return 样本标准差，至少 10 个连续有效样本；单位 bps/√秒。',
        '缺少官方 Chainlink strike 历史值。distance 使用 Binance 市场开始前最后成交价作代理，严格标注 proxy；不是官方到 strike 距离。',
        'spread 是 UP/DOWN 两个报价 spread 的均值。tau 单位秒；normalized proxy distance 为 |log(P/Pstart)|/(σ√tau)。','',
        '| 指标 | 旧窗口 mean | 新窗口 mean | 旧窗口 p10 / p50 / p90 / p99 | 新窗口 p10 / p50 / p90 / p99 |',
        '|---|---:|---:|---|---|']
    for k in METRICS:
        a=result['state']['reused']['metrics'][k];b=result['state']['confirmation']['metrics'][k]
        lines.append(f"| {k} | {a['mean']:.6g} | {b['mean']:.6g} | "+' / '.join(f'{v:.6g}' for v in a['p10_p50_p90_p99'])+' | '+' / '.join(f'{v:.6g}' for v in b['p10_p50_p90_p99'])+' |')
    for w,r in result['state'].items():
        lines += ['',f"{w}: ready {r['ready_fraction']:.2%}；|alpha|≥shock 占 ready {r['abs_alpha_ge_shock_fraction']:.2%}；|alpha|≥halt {r['abs_alpha_ge_halt_fraction']:.2%}；每市场去簇 shock {r['declustered_shocks_per_contract']:.3f}。",str(r['ready_regime_fraction'])]
    lines+=['','## Shock 后持续性与实际价格延续','',
        'shock onset 是相邻有效网格从 |alpha|<0.015 穿越到 ≥0.015；缺失后首次超阈值不算已知 onset。',
        '相邻事件至少间隔 5 秒，保留距到期至少 10 秒的事件；冲击含 HALT 大冲击，另提供 eligible SHOCK 子集。',
        '方向为 onset alpha 的符号，后续 BTC log return 与 PM anchor 变化乘此符号。PM 后续仅要求可用盘口，不要求后续策略 ready。',
        '只在同一合约内取未来值。alpha persistence 和价格 continuation 分别报告，不把模型同号误认为可成交盈利。','',
        '| horizon ms | 旧/新 alpha 同号率 | 旧/新 PM signed Δ（美分） | 旧/新 BTC signed return bps | 旧/新样本数 PM |',
        '|---|---|---|---|---|']
    for h in HORIZONS:
        a=result['state']['reused']['episodes']['horizons'][str(h)];b=result['state']['confirmation']['episodes']['horizons'][str(h)]
        lines.append(f"| {h} | {a['alpha_same_sign']['mean']:.2%} / {b['alpha_same_sign']['mean']:.2%} | {100*a['pm_signed_change']['mean']:.4f} / {100*b['pm_signed_change']['mean']:.4f} | {a['btc_signed_return_bps']['mean']:.5f} / {b['btc_signed_return_bps']['mean']:.5f} | {a['pm_signed_change']['n']} / {b['pm_signed_change']['n']} |")
    lines+=['','机器结果还包含逐日 state/episode 分布和 time-to-expiry 分层。相邻网格与事件相关，样本量不能当独立观察数。',
        '此诊断是对已知结果的事后归因，未调参，未新增独立确认窗口。','',
        '[完整机器结果](results.json) · [方法冻结清单](manifest.json) · [原实验](../README.md)','']
    (OUT/'README.md').write_text('\n'.join(lines))
    print(json.dumps(result['pnl'],indent=2),flush=True)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('phase',choices=['analyze','report']);parser.add_argument('--workers',type=int,default=16)
    a=parser.parse_args()
    if a.phase=='analyze':analyze(a.workers)
    else:report()


if __name__=='__main__':main()

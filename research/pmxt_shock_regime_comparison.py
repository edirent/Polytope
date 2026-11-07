#!/usr/bin/env python3
"""Supplement frozen replay diagnostics with strike coverage and day-block checks."""
from __future__ import annotations
import argparse
import concurrent.futures as futures
import json
from pathlib import Path
import numpy as np
import pmxt_btc15m_backtest as base
import pmxt_shock_fixed_tests as suite
import pmxt_shock_diagnostics as diag

OUT=diag.OUT


def strike_one(job):
    window,slug=job;root=OUT/window/'baseline'/slug;target=root/'strike_distance.npz'
    if target.exists():return
    data=suite.OLD if window=='reused' else suite.NEW
    r=json.loads((root/'case.json').read_text());start=r['start_ms']
    gamma=data/'gamma'/(str(start//1000)+'.json');g=json.loads(gamma.read_text())
    meta=g.get('eventMetadata',{});strike=meta.get('priceToBeat');final=meta.get('finalPrice')
    if strike is not None and final is not None and ('Up' if final>=strike else 'Down')!=r['winner']:
        raise ValueError('official metadata disagrees with winner')
    with np.load(root/'state.npz') as z:times=z['times'];ready=z['ready'];sigma=z['values'][:,1]/10000;tau=z['values'][:,2]
    info={'price_to_beat':strike,'final_price':final,'gamma_sha256':suite.sha(gamma),
          'observed_btc_price':'Binance BTCUSDT, not Chainlink BTCUSD','retrospective_only':True}
    if strike is None:
        distance=np.full(len(times),np.nan)
    else:
        source,recv,price,_,_=base._load_btc(data,start,start+900000)
        j=np.searchsorted(recv,times,side='right')-1;safe=np.maximum(j,0)
        distance=np.log(price[safe]/float(strike))*10000
        distance[(j<0)|(times-recv[safe]>1500)]=np.nan
    np.savez_compressed(target,signed_distance_bps=distance,
        abs_distance_sigma=np.abs(distance)/10000/(sigma*np.sqrt(tau)))
    base.save(root/'strike_audit.json',info)


def strikes(workers):
    jobs=[(w,f.parent.name) for w in ['reused','confirmation'] for f in sorted((OUT/w/'baseline').glob('*/state.npz'))]
    if len(jobs)!=1632:raise ValueError('baseline states incomplete')
    with futures.ProcessPoolExecutor(max_workers=workers) as pool:
        for i,_ in enumerate(pool.map(strike_one,jobs),1):
            if i%100==0:print('official strike',i,'/',len(jobs),flush=True)


def difference_interval(old,new,seed=20260508):
    """Rows are (day numerator, day denominator); weighted resampled day blocks."""
    old=np.asarray(old,dtype=float);new=np.asarray(new,dtype=float)
    if not len(old) or not len(new):return None
    rng=np.random.default_rng(seed)
    def draws(a):
        ix=rng.integers(0,len(a),size=(10000,len(a)));s=a[ix].sum(axis=1)
        return np.divide(s[:,0],s[:,1],out=np.full(10000,np.nan),where=s[:,1]>0)
    d=draws(new)-draws(old)
    return {'new_minus_old':float(new[:,0].sum()/new[:,1].sum()-old[:,0].sum()/old[:,1].sum()),
        'day_block_bootstrap_95pct':np.nanquantile(d,[.025,.975]).tolist(),
        'old_day_blocks':len(old),'new_day_blocks':len(new)}


def standardize(records,vol_edges):
    # Fixed expiry bins; old-window volatility quartiles; shock alpha magnitudes.
    tau_edges=[60,300,600];alpha_edges=[.02,.04]
    cells={}
    for r in records:
        if not r['eligible_shock']:continue
        v=r['onset_values'];cell=(int(np.searchsorted(tau_edges,v[2])),
            int(np.searchsorted(vol_edges,v[1])),int(np.searchsorted(alpha_edges,abs(r['alpha']))),int(r['alpha']>0))
        cells.setdefault(cell,[]).append(r)
    return cells


def report():
    main=json.loads((OUT/'results.json').read_text());result={'official_strike':{},'episode_groups':{},
        'day_block_comparisons':{},'standardized_eligible_shock':{},'coverage_checks':{}}
    all_records={};day_rows={}
    for w,expected in [('reused',960),('confirmation',672)]:
        distances=[];normdist=[];contracts=0;proxy_contracts=0;records=[];daily={}
        for root in sorted((OUT/w/'baseline').glob('btc-updown-*')):
            if not (root/'state.npz').exists():continue
            audit=json.loads((root/'strike_audit.json').read_text());case=json.loads((root/'case.json').read_text())
            if audit['price_to_beat'] is not None:contracts+=1
            if case['strike_proxy']['strike_proxy_price'] is not None:proxy_contracts+=1
            with np.load(root/'state.npz') as z:ready=z['ready'];values=z['values'];times=z['times'];alpha=z['alpha']
            with np.load(root/'strike_distance.npz') as z:dist=z['signed_distance_bps'];norm=z['abs_distance_sigma']
            distances.append(np.abs(dist[ready]));normdist.append(norm[ready])
            d=case['day_utc'];daily.setdefault(d,[])
            for r in json.loads((root/'episodes.json').read_text()):
                i=int((r['now_ms']-case['start_ms'])//50);r['day_utc']=d;r['slug']=root.name
                r['onset_values']=values[i].tolist();r['official_distance_bps']=float(dist[i]) if np.isfinite(dist[i]) else None
                records.append(r);daily[d].append(r)
        all_records[w]=records;day_rows[w]=daily
        result['official_strike'][w]={'markets_with_strike':contracts,'markets_total':expected,
            'contract_coverage':contracts/expected,'abs_binance_to_official_strike_bps':diag.describe(np.concatenate(distances)),
            'abs_binance_to_official_strike_sigma':diag.describe(np.concatenate(normdist)),
            'binance_start_proxy_markets':proxy_contracts,'binance_start_proxy_coverage':proxy_contracts/expected,
            'limitation':'Official priceToBeat vs Binance spot; no full historical Chainlink intramarket price path.'}
        result['episode_groups'][w]={}
        for name,subset in [('all',records),('eligible_shock',[r for r in records if r['eligible_shock']]),
                            ('positive',[r for r in records if r['alpha']>0]),('negative',[r for r in records if r['alpha']<0])]:
            result['episode_groups'][w][name]=diag.episode_summary(subset)
        result['coverage_checks'][w]={'all_state_volatility_finite':all(
            np.isfinite(r['onset_values'][1]) for r in records),'episode_onsets':len(records)}
    for i,k in enumerate(diag.METRICS):
        pairs={}
        for w in ['reused','confirmation']:
            pairs[w]=[(v['metrics'][k]['mean']*v['metrics'][k]['n'],v['metrics'][k]['n'])
                for v in main['state'][w]['daily'].values()]
        result['day_block_comparisons'][k]=difference_interval(pairs['reused'],pairs['confirmation'])
    for group in ['all','eligible_shock']:
        result['day_block_comparisons'][group]={}
        for h in diag.HORIZONS:
            result['day_block_comparisons'][group][str(h)]={}
            for k in ['alpha_same_sign','pm_signed_change','btc_signed_return_bps']:
                pairs={}
                for w in ['reused','confirmation']:
                    pairs[w]=[]
                    for records in day_rows[w].values():
                        a=[r['horizons'][str(h)][k] for r in records if
                           (group=='all' or r['eligible_shock']) and r['horizons'][str(h)][k] is not None]
                        if a:pairs[w].append((sum(a),len(a)))
                result['day_block_comparisons'][group][str(h)][k]=difference_interval(pairs['reused'],pairs['confirmation'])
    # Common state support, old event mix weighting, no changes to strategy.
    old_vol=[r['onset_values'][1] for r in all_records['reused'] if r['eligible_shock']]
    vol_edges=np.quantile(old_vol,[.25,.5,.75]);cells={w:standardize(all_records[w],vol_edges) for w in all_records}
    common=sorted(set(cells['reused'])&set(cells['confirmation']))
    total_old=sum(len(v) for v in cells['reused'].values());total_new=sum(len(v) for v in cells['confirmation'].values())
    standardized={'old_volatility_quartile_edges_bps':vol_edges.tolist(),'common_cells':len(common),
        'expiry_edges_seconds':[60,300,600],'alpha_edges':[.02,.04],'direction_stratified':True,
        'old_common_events':sum(len(cells['reused'][c]) for c in common),'old_events':total_old,
        'new_common_events':sum(len(cells['confirmation'][c]) for c in common),'new_events':total_new,'horizons':{}}
    for h in diag.HORIZONS:
        standardized['horizons'][str(h)]={}
        for k in ['alpha_same_sign','pm_signed_change','btc_signed_return_bps']:
            estimates=[];details=[]
            for c in common:
                a=[r['horizons'][str(h)][k] for r in cells['reused'][c] if r['horizons'][str(h)][k] is not None]
                b=[r['horizons'][str(h)][k] for r in cells['confirmation'][c] if r['horizons'][str(h)][k] is not None]
                if not a or not b:continue
                weight=len(cells['reused'][c]);estimates.append((weight,float(np.mean(a)),float(np.mean(b))))
                details.append({'cell':c,'old_valid':len(a),'new_valid':len(b),'weight':weight})
            denom=sum(t[0] for t in estimates)
            standardized['horizons'][str(h)][k]={'reused_mean':sum(w*a for w,a,b in estimates)/denom,
                'confirmation_mean_at_old_mix':sum(w*b for w,a,b in estimates)/denom,
                'old_weight_events':denom,'cells':details}
    result['standardized_eligible_shock']=standardized
    base.save(OUT/'regime_comparison.json',result)
    lines=['# 官方 strike、日块不确定性与共同状态对照','',
        'Gamma 缓存 eventMetadata.priceToBeat 是官方 strike 的事后元数据。两窗口 available finalPrice 与原赢家校验无冲突。',
        '距离中的动态 BTC 价格仍为 Binance BTCUSDT，不是假定拥有完整 Chainlink BTC/USD 历史路径；跨源基差可能影响距离。',
        '缺失 strike 不用代理值填入官方列。另有独立的 Binance 开盘代理口径；开盘附近价格超过 1500ms 过旧时，该代理也记缺失。','',
        '| 窗口 | strike 覆盖 | 绝对 log 距离平均 bps | p50 bps | 平均 σ√tau 单位 |',
        '|---|---:|---:|---:|---:|']
    for w,r in result['official_strike'].items():
        a=r['abs_binance_to_official_strike_bps'];b=r['abs_binance_to_official_strike_sigma']
        lines.append(f"| {w} | {r['markets_with_strike']}/{r['markets_total']} | {a['mean']:.4f} | {a['p10_p50_p90_p99'][1]:.4f} | {b['mean']:.4f} |")
    lines+=['', 'Binance 开盘代理有效市场：'+ '；'.join(f"{w} {r['binance_start_proxy_markets']}/{r['markets_total']}" for w,r in result['official_strike'].items())+'。所有距离均值/分位数只用有效样本，不把缺失补零。']
    lines+=['','日块 bootstrap 用 10 个旧窗口日块和 7 个新窗口日块分别重采样，计算新减旧；这不是网格独立样本的显著性检验。','',
        '| 状态指标 | 新减旧 | 日块 bootstrap 95% |','|---|---:|---|']
    for k in diag.METRICS:
        r=result['day_block_comparisons'][k]
        lines.append(f"| {k} | {r['new_minus_old']:.6g} | {r['day_block_bootstrap_95pct'][0]:.6g} / {r['day_block_bootstrap_95pct'][1]:.6g} |")
    lines+=['','仅 eligible SHOCK onset（0.015≤|alpha|<0.04），shock 后结果：','',
        '| horizon ms | 旧/新 PM Δ ¢ | PM 新减旧 95% ¢ | 旧/新 BTC return bps | BTC 新减旧 95% bps |',
        '|---|---|---|---|---|']
    for h in diag.HORIZONS:
        a=result['episode_groups']['reused']['eligible_shock']['horizons'][str(h)];b=result['episode_groups']['confirmation']['eligible_shock']['horizons'][str(h)]
        ci=result['day_block_comparisons']['eligible_shock'][str(h)]
        pci=ci['pm_signed_change']['day_block_bootstrap_95pct'];bci=ci['btc_signed_return_bps']['day_block_bootstrap_95pct']
        lines.append(f"| {h} | {100*a['pm_signed_change']['mean']:.4f} / {100*b['pm_signed_change']['mean']:.4f} | {100*pci[0]:.4f} / {100*pci[1]:.4f} | {a['btc_signed_return_bps']['mean']:.5f} / {b['btc_signed_return_bps']['mean']:.5f} | {bci[0]:.5f} / {bci[1]:.5f} |")
    lines+=['','再将 eligible shock 按 tau 四档、旧窗口 RV 四分位、alpha 两档和信号正负方向分层；只比较共同支持，按旧窗口事件数量加权。',
        '这是事后状态标准化，用于区分 mix shift 与条件延续变化，不是因果识别，也没有训练或选择新交易阈值。',
        f"共同支持 {standardized['common_cells']} 个非空分层，覆盖旧 {standardized['old_common_events']}/{total_old}、新 {standardized['new_common_events']}/{total_new} 个 eligible shock。",'',
        '| horizon ms | 标准化旧/新 PM Δ ¢ | 标准化旧/新 BTC return bps | 标准化旧/新 alpha 同号率 |',
        '|---|---|---|---|']
    for h in diag.HORIZONS:
        r=standardized['horizons'][str(h)];p=r['pm_signed_change'];b=r['btc_signed_return_bps'];a=r['alpha_same_sign']
        lines.append(f"| {h} | {100*p['reused_mean']:.4f} / {100*p['confirmation_mean_at_old_mix']:.4f} | {b['reused_mean']:.5f} / {b['confirmation_mean_at_old_mix']:.5f} | {a['reused_mean']:.2%} / {a['confirmation_mean_at_old_mix']:.2%} |")
    lines+=['','[机器结果](regime_comparison.json) · [收益与状态主报告](README.md)','']
    (OUT/'regime_comparison.md').write_text('\n'.join(lines))
    report=OUT/'README.md'
    # The initial declared diagnostic used a proxy; supplement actual metadata
    # without modifying any replay or its declared matching/state rules.
    text=report.read_text().replace('缺少官方 Chainlink strike 历史值。distance 使用 Binance 市场开始前最后成交价作代理，严格标注 proxy；不是官方到 strike 距离。',
        '本表 distance 使用 Binance 市场开始前最后成交价作代理；另已从 Gamma 原始缓存取得部分市场的官方 priceToBeat，真实 strike 距离与缺失覆盖见补充报告。')
    link='[官方 strike、日块区间与共同状态比较](regime_comparison.md)'
    if link not in text:text+='\n'+link+'\n'
    report.write_text(text)
    print(json.dumps({k:v for k,v in result.items() if k!='standardized_eligible_shock'},indent=2))


def main():
    p=argparse.ArgumentParser();p.add_argument('phase',choices=['strikes','report']);p.add_argument('--workers',type=int,default=8);a=p.parse_args()
    if a.phase=='strikes':strikes(a.workers)
    else:report()


if __name__=='__main__':main()

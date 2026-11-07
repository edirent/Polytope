#!/usr/bin/env python3
"""One frozen-baseline experiment: execution conditional on shock age."""
from __future__ import annotations
import argparse
import bisect
import concurrent.futures as futures
import gzip
import json
import subprocess
import tempfile
from pathlib import Path
import numpy as np
import pmxt_btc15m_backtest as base
import pmxt_shock_fixed_tests as suite
import build_pmxt_fill_observer as observer

OUT=observer.ROOT
BINS=[('shock 前',-1000,0),('0–100ms',0,100),('100–250ms',100,250),
      ('250–500ms',250,500),('0.5–1s',500,1000),('1–2s',1000,2000),('2–5s',2000,5000)]
NEG=-float('inf')


def segments(shocks):
    """Disjoint windows: most recent shock wins; pre only outside its 5s tail."""
    result=[]
    for i,s in enumerate(shocks):
        t=s['now_ms'];key=(t,s['sequence'])
        previous_tail=shocks[i-1]['now_ms']+5000 if i else -10**20
        lo=(max(t-1000,previous_tail),NEG)
        if lo<key:result.append((lo,key,i,0))
        next_key=(shocks[i+1]['now_ms'],shocks[i+1]['sequence']) if i+1<len(shocks) else (10**20,NEG)
        for b,(_,a,z) in enumerate(BINS[1:],1):
            lo=key if a==0 else (t+a,NEG);hi=min((t+z,NEG),next_key)
            if lo<hi:result.append((lo,hi,i,b))
    result.sort()
    for a,b in zip(result,result[1:]):
        if a[1]>b[0]:raise ValueError('overlapping shock-age windows')
    return result


def blank():
    return {'at_risk_order_bucket_samples':0,'filled_order_bucket_samples':0,'active_order_ms':0,
        'fill_events':0,'fill_lots':0,'markout_sum':[0.,0.,0.],'markout_lots':[0,0,0],
        'spread_sum_tick_lots':0,'spread_lots':0,'cancel_pending_lots':0,
        'against_shock_lots':0,'orders_created_before_shock_fill_lots':0}


def aggregate(orders,shocks,fills):
    rows=[blank() for _ in BINS];ss=segments(shocks);starts=[s[0] for s in ss];ends=[s[1] for s in ss]
    exposure={};order_map={o['id']:o for o in orders}
    for o in orders:
        if o['active_ms']<0 or o['end_ms']<o['active_ms']:raise ValueError('incomplete accepted order lifecycle')
        lo=(o['active_ms'],o['active_sequence']+.25)
        hi=(o['end_ms'],o['end_sequence']+(.25 if o['ended_by_fill'] else -.25))
        if hi<=lo:raise ValueError('invalid order lifetime')
        first=bisect.bisect_right(ends,lo)
        for j in range(first,len(ss)):
            a,z,shock,b=ss[j]
            if a>=hi:break
            left=max(lo,a);right=min(hi,z)
            if left<right:
                sample=(o['id'],shock,b)
                if sample in exposure:raise ValueError('duplicate order-bucket exposure')
                exposure[sample]=False;rows[b]['at_risk_order_bucket_samples']+=1
                rows[b]['active_order_ms']+=max(0,right[0]-left[0])
    excluded=0;excluded_lots=0;same_ms_pre=0
    for f in fills:
        key=(f['event_ms'],f['sequence']);i=bisect.bisect_right(starts,key)-1
        if i<0 or key>=ss[i][1]:excluded+=1;excluded_lots+=f['quantity_lots'];continue
        _,_,shock,b=ss[i];sample=(f['order_id'],shock,b)
        if b==0 and f['event_ms']==shocks[shock]['now_ms']:same_ms_pre+=1
        if sample not in exposure:raise ValueError('filled order absent from actual active risk set')
        if not exposure[sample]:rows[b]['filled_order_bucket_samples']+=1;exposure[sample]=True
        r=rows[b];q=f['quantity_lots'];r['fill_events']+=1;r['fill_lots']+=q
        for h,mo in enumerate(f['markout']):
            if mo is not None:r['markout_sum'][h]+=q*mo;r['markout_lots'][h]+=q
        if f['visible_spread_tick'] is not None:
            r['spread_sum_tick_lots']+=q*f['visible_spread_tick'];r['spread_lots']+=q
        if f['cancel_pending']:r['cancel_pending_lots']+=q
        sign=1 if f['channel'] in (0,3) else -1
        if sign*shocks[shock]['alpha']<0:r['against_shock_lots']+=q
        if order_map[f['order_id']]['created_ms']<shocks[shock]['now_ms']:
            r['orders_created_before_shock_fill_lots']+=q
    if sum(r['fill_events'] for r in rows)+excluded!=len(fills):raise ValueError('fill classification did not reconcile')
    return rows,{'excluded_fill_events':excluded,'excluded_fill_lots':excluded_lots,
        'shock_onsets':len(shocks),'eligible_onsets':sum(abs(s['alpha'])<.04 for s in shocks),
        'same_timestamp_pre_shock_fills':same_ms_pre}


def one(job):
    window,slug=job;dest=OUT/window/slug;target=dest/'case.json'
    if target.exists():return json.loads(target.read_text())
    original=suite.OUT/window/'baseline'/slug;summary=json.loads((original/'summary.json').read_text())
    data=suite.OLD if window=='reused' else suite.NEW;dest.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fill-audit-',dir=OUT/'temporary') as td:
        audit=Path(td)/'execution.jsonl'
        run=subprocess.run([str((OUT/'run_fill_observer').resolve()),'--config',str(original/'config.json'),
            '--events',str(data/'normalized'/slug/'events.jsonl'),'--model',str(suite.OUT/'frozen_model.json'),
            '--settlement',summary['winner'].lower(),'--audit',str(audit)],capture_output=True,text=True,timeout=300)
        if run.returncode:raise RuntimeError(run.stderr[-2000:])
        replayed=json.loads(run.stdout)
        if any(summary[k]!=v for k,v in replayed.items()):raise ValueError('observer changed frozen replay output')
        orders=[];shocks=[];fills=[]
        with audit.open() as f:
            for line in f:
                r=json.loads(line)
                {'order':orders,'shock':shocks,'fill':fills}[r['type']].append(r)
        if len(orders)!=summary['submitted'] or len(fills)!=summary['fills']:raise ValueError('audit counts do not reconcile')
        with gzip.open(suite.OUT/'diagnostics'/window/'baseline'/slug/'fills.jsonl.gz','rt') as f:old=[json.loads(line) for line in f]
        keys=['event_ms','channel','price_tick','quantity_lots','cancel_pending','markout']
        if any(any(a[k]!=b[k] for k in keys) for a,b in zip(fills,old)) or len(fills)!=len(old):
            raise ValueError('observer fills differ from original fills')
        with audit.open('rb') as src,gzip.open(dest/'execution.jsonl.gz','wb') as dst:dst.write(src.read())
    rows,coverage=aggregate(orders,shocks,fills)
    result={'window':window,'slug':slug,'start_ms':summary['start_ms'],'day_utc':base.dt.datetime.fromtimestamp(summary['start_ms']/1000,base.UTC).date().isoformat(),
        'bins':rows,'coverage':coverage,'orders':len(orders),'fills':len(fills),'fill_lots':sum(f['quantity_lots'] for f in fills),
        'markout_sum':[sum(f['quantity_lots']*f['markout'][h] for f in fills if f['markout'][h] is not None) for h in range(3)],
        'markout_lots':summary['markout_quantity'],'original_summary_exact_match':True,'original_fill_list_exact_match':True,
        'original_summary_sha256':suite.sha(original/'summary.json')}
    base.save(target,result);return result


def run(workers):
    frozen=json.loads((suite.OUT/'frozen_manifest.json').read_text());build=json.loads((OUT/'observer_build.json').read_text())
    if suite.sha(Path('build/run_btc15m_lead_lag_replay'))!=frozen['executable_sha256']:raise ValueError('original executable changed')
    if suite.sha(suite.OUT/'frozen_model.json')!=frozen['model_sha256']:raise ValueError('model changed')
    if suite.sha(OUT/'run_fill_observer')!=build['observer_sha256']:raise ValueError('observer executable changed')
    (OUT/'temporary').mkdir(parents=True,exist_ok=True)
    manifest={'experiment':'actual-fill conditional shock-age, frozen baseline only','bins':BINS,
        'windows':{'reused':'2026-04-21/2026-05-01','confirmation':'2026-05-01/2026-05-08'},
        'candidate':'shock_entry','model_sha256':frozen['model_sha256'],'frozen_executable_sha256':frozen['executable_sha256'],
        'observer_sha256':build['observer_sha256'],'source_sha256':suite.sha(Path(__file__)),
        'shock_definition':'All actual decision-time ready crossings |alpha| from <0.015 to >=0.015; prior decision ready and gap <=1500ms. Includes HALT-strength shock; no declustering.',
        'assignment':'Most recent preceding onset wins for 0..5s; pre is next onset -1s..0 only outside previous onset 5s tail. Each fill used at most once.',
        'same_ms_ordering':'Execution callback sequence distinguishes source fill before observed shock at same millisecond.',
        'fill_probability':'Order x shock x bucket risk-set samples with >=1 actual fill / samples with an exchange-active order overlapping bucket.',
        'spread':'Filled quantity weighted token bid-ask spread visible just before actual fill; unhealthy/stale >1500ms is missing.',
        'markout':'Original per-fill source-horizon labels at 100ms/1s/5s; quantity weighted; missing future labels excluded, never zero-filled.',
        'fixed_strategy':frozen['strategy_config'],'retraining':False,'additional_latency_profiles':False}
    p=OUT/'manifest.json'
    if p.exists() and json.loads(p.read_text())!=manifest:raise ValueError('experiment manifest changed')
    base.save(p,manifest)
    jobs=[(w,f.parent.name) for w in ['reused','confirmation'] for f in sorted((suite.OUT/w/'baseline').glob('*/summary.json'))]
    with futures.ProcessPoolExecutor(max_workers=workers) as pool:
        for i,_ in enumerate(pool.map(one,jobs),1):
            if i%50==0:print('fill conditional',i,'/',len(jobs),flush=True)
    base.save(OUT/'completion.json',{'cases':len(jobs),'exact_summary_matches':len(jobs),'exact_fill_list_matches':len(jobs)})


def combined(cases):
    bins=[blank() for _ in BINS]
    for case in cases:
        for r,a in zip(bins,case['bins']):
            for k,v in a.items():
                if isinstance(v,list):r[k]=[x+y for x,y in zip(r[k],v)]
                else:r[k]+=v
    for r in bins:
        n=r['at_risk_order_bucket_samples'];q=r['fill_lots']
        r['fill_probability']=r['filled_order_bucket_samples']/n if n else None
        r['markout_cents']=[100*s/v if v else None for s,v in zip(r['markout_sum'],r['markout_lots'])]
        r['spread_cents']=r['spread_sum_tick_lots']/r['spread_lots']/10000 if r['spread_lots'] else None
        r['fill_events_per_active_order_second']=r['fill_events']/(r['active_order_ms']/1000) if r['active_order_ms'] else None
        r['cancel_pending_fill_share']=r['cancel_pending_lots']/q if q else None
        r['against_shock_fill_share']=r['against_shock_lots']/q if q else None
        r['preexisting_order_fill_share']=r['orders_created_before_shock_fill_lots']/q if q else None
    return bins


def report():
    result={'methodology':json.loads((OUT/'manifest.json').read_text()),'windows':{}}
    for w,expected in [('reused',960),('confirmation',672)]:
        cases=[json.loads(f.read_text()) for f in sorted((OUT/w).glob('*/case.json'))]
        if len(cases)!=expected:raise ValueError('incomplete cases')
        bins=combined(cases)
        result['windows'][w]={'contracts':expected,'bins':bins,
            'all_actual_fills':sum(r['fills'] for r in cases),'all_actual_fill_lots':sum(r['fill_lots'] for r in cases),
            'included_actual_fills':sum(r['fill_events'] for r in bins),'included_actual_fill_lots':sum(r['fill_lots'] for r in bins),
            'excluded_actual_fills':sum(r['coverage']['excluded_fill_events'] for r in cases),
            'shock_onsets':sum(r['coverage']['shock_onsets'] for r in cases),
            'daily':{d:combined([r for r in cases if r['day_utc']==d]) for d in sorted({r['day_utc'] for r in cases})}}
        assert result['windows'][w]['included_actual_fills']+result['windows'][w]['excluded_actual_fills']==result['windows'][w]['all_actual_fills']
        for h in range(3):
            q=sum(r['markout_lots'][h] for r in cases);s=sum(r['markout_sum'][h] for r in cases)
            prior=json.loads((suite.OUT/'results.json').read_text())['windows'][w]['baseline']
            assert q==prior['markout_quantity'][h]
            assert abs(s/q-prior['weighted_markout_100ms_1s_5s'][h])<1e-12
    base.save(OUT/'results.json',result)
    lines=['# 按距 shock 时间分桶：仅实际模拟成交','',
        '固定两窗口 baseline；不是 shock 事件平均价格响应。每个实际成交只进入一个桶。','',
        'Shock 是实际策略决策时 alpha 从绝对值 <0.015 穿越至 ≥0.015，含达到 HALT 强度的冲击；不使用 5 秒去簇或未来价格选择事件。',
        'shock 前取前 1 秒；post 按最近一次 onset，新的 onset 重置时钟。pre 只取未落在前一个 shock 后 5 秒内的区间，避免重叠重复计算。',
        '同一毫秒内公共成交先于 shock 决策时，按回放执行顺序归为 shock 前，不假定机器人已经看见信号。','',
        'Fill probability = 有实际成交的订单×shock×桶样本 / 有交易所侧有效挂单的订单×shock×桶样本。',
        '未确认的发单不计入风险集，撤单等待但仍有效的订单计入；部分成交后剩余订单仍有效。',
        '不同桶时长不同，这个概率不能直接当相同期限 hazard，因此附单位有效挂单时间的成交率。',
        'markout 和 spread 只在实际成交上按股数加权。spread 是成交前本方可见的对应 token spread，过旧/不可用记缺失；不是假定测得交易所实时盘口。',
        'markout 使用原回放的 PM source-horizon 标签，不把缺失未来价格填零。','']
    def fmt(v):return '—' if v is None else f'{v:.4f}'
    for w,r in result['windows'].items():
        lines += [f'## {w}：{r["contracts"]} 个市场','',
            '| 时间距 shock | Fill probability | 100ms markout ¢ | 1s markout ¢ | 5s markout ¢ | Spread ¢ |',
            '|---|---:|---:|---:|---:|---:|']
        for (name,_,_),b in zip(BINS,r['bins']):
            probability='—' if b['fill_probability'] is None else f'{100*b["fill_probability"]:.4f}%'
            lines.append('| '+name+' | '+probability+' | '+' | '.join(fmt(v) for v in b['markout_cents']+[b['spread_cents']])+' |')
        lines += ['', '| 时间距 shock | 成交订单样本 / 有效挂单样本 | 成交次数 / 股数 | 有效 100ms / 1s / 5s 股数 | 成交次数 / 挂单秒 | 撤单等待成交股数占比 | 与 shock 反向成交占比 |',
            '|---|---:|---:|---|---:|---:|---:|']
        for (name,_,_),b in zip(BINS,r['bins']):
            lines.append(f'| {name} | {b["filled_order_bucket_samples"]}/{b["at_risk_order_bucket_samples"]} | {b["fill_events"]}/{b["fill_lots"]} | '+
                ' / '.join(str(q) for q in b['markout_lots'])+f' | {fmt(b["fill_events_per_active_order_second"])} | {fmt(100*b["cancel_pending_fill_share"] if b["cancel_pending_fill_share"] is not None else None)}% | {fmt(100*b["against_shock_fill_share"] if b["against_shock_fill_share"] is not None else None)}% |')
        lines += ['',f'纳入 {r["included_actual_fills"]}/{r["all_actual_fills"]} 次实际成交、{r["included_actual_fill_lots"]}/{r["all_actual_fill_lots"]} 股；其余成交在这些事件窗口外，另列审计，不挪入 shock 前。',
            f'实际决策时穿越事件 {r["shock_onsets"]} 个；逐日同口径表见 results.json。','']
    lines += ['[机器结果](results.json) · [方法清单](manifest.json) · [观察程序构建](observer_build.json)',
        '[原收益诊断](../diagnostics/findings.md)','']
    (OUT/'README.md').write_text('\n'.join(lines));print(json.dumps(result['windows'],indent=2))


def main():
    p=argparse.ArgumentParser();p.add_argument('phase',choices=['run','report']);p.add_argument('--workers',type=int,default=16);a=p.parse_args()
    if a.phase=='run':run(a.workers)
    else:report()


if __name__=='__main__':main()

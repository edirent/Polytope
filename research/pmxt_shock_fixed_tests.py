#!/usr/bin/env python3
"""Freeze shock-entry; separate reused-window stresses from unseen confirmation."""
from __future__ import annotations
import argparse
import concurrent.futures as futures
import datetime as dt
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np
import pmxt_btc15m_backtest as base

OLD=Path('runs/pmxt_btc15m_20260417_20260430')
NEW=Path('runs/pmxt_shock_fixed_20260501_20260507')
OUT=Path('runs/pmxt_shock_fixed_tests_20261006')
PROFILES={
    'baseline':{'btc_ms':20,'submit_ms':25,'cancel_ms':100,'notice_ms':50,'queue':1.0},
    'cancel200':{'btc_ms':20,'submit_ms':25,'cancel_ms':200,'notice_ms':50,'queue':1.0},
    'cancel500':{'btc_ms':20,'submit_ms':25,'cancel_ms':500,'notice_ms':50,'queue':1.0},
    'btc100':{'btc_ms':100,'submit_ms':25,'cancel_ms':100,'notice_ms':50,'queue':1.0},
    'btc250':{'btc_ms':250,'submit_ms':25,'cancel_ms':100,'notice_ms':50,'queue':1.0},
    'submit100':{'btc_ms':20,'submit_ms':100,'cancel_ms':100,'notice_ms':50,'queue':1.0},
    'notice200':{'btc_ms':20,'submit_ms':25,'cancel_ms':100,'notice_ms':200,'queue':1.0},
    'queue2':{'btc_ms':20,'submit_ms':25,'cancel_ms':100,'notice_ms':50,'queue':2.0},
    'joint':{'btc_ms':100,'submit_ms':100,'cancel_ms':200,'notice_ms':200,'queue':2.0},
}
CONFIRM_PROFILES=['baseline','cancel200','joint']


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def freeze():
    OUT.mkdir(parents=True,exist_ok=True)
    model=OLD/'models/external.json';artifact=json.loads(model.read_text())
    if artifact['synthetic'] or artifact['pm_only']:raise ValueError('wrong model provenance')
    expected=json.loads((OLD/'models/fit_manifest.json').read_text())['models']['external']['sha256']
    if sha(model)!=expected:raise ValueError('original model changed')
    fixture=json.loads((OLD/'markets.json').read_text())[384]
    cfg=base.replay_config(fixture,'shock_entry');cfg.pop('market')
    selected=json.loads((OLD/'results.json').read_text())['profiles']['shock_entry']
    manifest={'candidate':'shock_entry','model_sha256':expected,'trained_until_ms':artifact['trained_until_ms'],
        'executable_sha256':sha(Path('build/run_btc15m_lead_lag_replay')),'strategy_config':cfg,
        'selection_window':'2026-04-21/2026-05-01','selection_window_baseline_pnl_usd':selected['pnl_usd'],
        'confirmation_window':'2026-05-01/2026-05-08','profiles':PROFILES,'confirmation_profiles':CONFIRM_PROFILES,
        'retraining_permitted':False,'strategy_parameters_tuned_in_suite':False,
        'latencies_are_assumptions':True,'reward_usd':0,'operating_cost_usd':0,'terminal_taker':False}
    path=OUT/'frozen_manifest.json'
    if path.exists() and json.loads(path.read_text())!=manifest:raise ValueError('frozen experiment manifest changed')
    base.save(path,manifest)
    shutil.copyfile(model,OUT/'frozen_model.json')
    preset=base.replay_config(fixture,'shock_entry');preset['btc_receive_delay_ms']=20
    preset['model_path']=str(OUT/'frozen_model.json')
    base.save(Path('config/btc15m_shock_entry.pmxt_fixed.json'),preset)
    print('frozen shock-entry',expected,flush=True)


def configuration(m,p):
    cfg=base.replay_config(m,'shock_entry');assumptions=PROFILES[p]
    cfg['btc_receive_delay_ms']=assumptions['btc_ms'];cfg['queue_factor']=assumptions['queue']
    delays=cfg['latency_assumptions_ms'];delays['submit']=assumptions['submit_ms']
    delays['cancel']=assumptions['cancel_ms'];delays['fill_notice']=assumptions['notice_ms']
    return cfg


def prepare_confirmation(workers):
    """Normalize only after every targeted hour for that contract is immutable."""
    markets=json.loads((NEW/'markets.json').read_text())
    hours=json.loads((NEW/'hourly_coverage.json').read_text());required={}
    for m in markets:
        created=int(dt.datetime.fromisoformat(m['created_at'].replace('Z','+00:00')).timestamp()*1000)
        need=[]
        for row in hours:
            stamp=dt.datetime.fromisoformat(row['hour_utc']);h=int(stamp.timestamp()*1000)
            if h<=created<h+3600000 or h<=created+3600000<h+3600000 or m['start_ms']-3600000<=h<m['expiry_ms']+3600000:
                need.append(stamp.strftime('%Y-%m-%dT%H'))
        required[m['slug']]=need
    done={m['slug'] for m in markets if (NEW/'normalized'/m['slug']/'coverage.json').exists()};pending={};submitted=set(done);deadline=time.monotonic()+7200
    with futures.ProcessPoolExecutor(max_workers=workers) as pool:
        while len(done)<len(markets):
            base.shard_hours(NEW)
            for m in markets:
                if m['slug'] not in submitted and all((NEW/'filtered'/(h+'.sharded')).exists() for h in required[m['slug']]):
                    pending[pool.submit(base.normalize_market,(str(NEW),m))]=m;submitted.add(m['slug'])
            for task in list(pending):
                if not task.done():continue
                m=pending.pop(task);r=task.result()
                if 'error'in r:raise ValueError(r)
                done.add(m['slug'])
            print('confirmation normalized',len(done),'/',len(markets),'pending',len(pending),flush=True)
            if time.monotonic()>deadline:raise TimeoutError('input preparation timeout')
            if len(done)<len(markets):time.sleep(5)
    coverage=[json.loads((NEW/'normalized'/m['slug']/'coverage.json').read_text()) for m in markets]
    base.save(NEW/'market_coverage.json',coverage)
    base.repair_expiry_tail(NEW,4)


def run_suite(window,workers):
    manifest=json.loads((OUT/'frozen_manifest.json').read_text())
    executable=Path('build/run_btc15m_lead_lag_replay').resolve();model=OUT/'frozen_model.json'
    if sha(executable)!=manifest['executable_sha256'] or sha(model)!=manifest['model_sha256']:
        raise ValueError('frozen model or executable changed')
    data=OLD if window=='reused' else NEW
    markets=json.loads((data/'markets.json').read_text())
    if window=='reused':markets=[m for m in markets if m['start_ms']>=manifest['trained_until_ms']]
    profiles=list(PROFILES) if window=='reused' else CONFIRM_PROFILES
    def one(job):
        m,p=job;dest=OUT/window/p/m['slug'];result=dest/'summary.json';cfg=configuration(m,p)
        for key in ('candidate','quote','regime','accounting','terminal_reduce_request_ms','decision_interval_ms'):
            if cfg[key]!=manifest['strategy_config'][key]:raise ValueError('fixed strategy parameter changed: '+key)
        if result.exists():
            r=json.loads(result.read_text())
            if r['model_sha256']!=manifest['model_sha256'] or r['executable_sha256']!=manifest['executable_sha256'] or r['config']!=cfg:
                raise ValueError('incompatible cached replay '+str(result))
            return r
        base.save(dest/'config.json',cfg)
        events=data/'normalized'/m['slug']/'events.jsonl';deadline=time.monotonic()+3600
        while not events.with_name('expiry_tail.json').exists():
            if time.monotonic()>deadline:raise TimeoutError('normalization incomplete '+m['slug'])
            time.sleep(2)
        cmd=[str(executable),'--config',str(dest/'config.json'),'--events',str(events),
             '--model',str(model),'--settlement',m['winner'].lower()]
        if m==markets[0]:cmd+=['--trace',str(dest/'trace.jsonl')]
        run=subprocess.run(cmd,capture_output=True,text=True,timeout=240)
        if run.returncode:raise RuntimeError(run.stderr[-1000:])
        r=json.loads(run.stdout)
        if r['synthetic'] or not r['model_loaded'] or r['final_reserved_lots'] or r['cash_tick']!=r['known_cash_tick'] or r['up_lots']!=r['known_up_lots'] or r['down_lots']!=r['known_down_lots']:
            raise ValueError('invalid provenance or ledger')
        if window=='reused' and p=='baseline':
            original=json.loads((OLD/'backtests/shock_entry'/m['slug']/'summary.json').read_text())
            if any(original[k]!=v for k,v in r.items()):raise ValueError('frozen baseline differs from prior run')
        r.update({'slug':m['slug'],'start_ms':m['start_ms'],'winner':m['winner'],'profile':p,'window':window,
            'config':cfg,'model_sha256':manifest['model_sha256'],'executable_sha256':manifest['executable_sha256']})
        base.save(result,r);return r
    jobs=[(m,p) for m in markets for p in profiles];results=[]
    with futures.ThreadPoolExecutor(max_workers=workers) as pool:
        pending={pool.submit(one,j):j for j in jobs}
        for task in futures.as_completed(pending):
            m,p=pending[task]
            try:r=task.result()
            except Exception as e:r={'slug':m['slug'],'profile':p,'error':str(e)}
            results.append(r)
            if len(results)%100==0:print(window,len(results),'/',len(jobs),'errors',sum('error'in x for x in results),flush=True)
    base.save(OUT/(window+'_batch.json'),results)
    if any('error'in r for r in results):raise RuntimeError('replay errors in '+window)


def summarize(rows):
    pnl=np.array([r['settlement_pnl_tick']/1e6 for r in rows]);daily={}
    for r in rows:
        day=dt.datetime.fromtimestamp(r['start_ms']/1000,base.UTC).strftime('%Y-%m-%d')
        daily[day]=daily.get(day,0)+r['settlement_pnl_tick']/1e6
    q=np.sum([r['markout_quantity'] for r in rows],axis=0)
    marks=[sum((r['quantity_weighted_markout'][i] or 0)*r['markout_quantity'][i] for r in rows)/int(q[i]) if q[i] else None for i in range(3)]
    values=np.array(list(daily.values()));rng=np.random.default_rng(20260501)
    draws=values[rng.integers(0,len(values),size=(10000,len(values)))].sum(axis=1)
    gross_profit=float(np.sum(pnl[pnl>0]));gross_loss=float(-np.sum(pnl[pnl<0]));curve=np.r_[0,np.cumsum(pnl)]
    submitted=sum(r['submitted'] for r in rows)
    return {'contracts':len(rows),'pnl_usd':float(pnl.sum()),'pnl_per_contract_usd':float(np.mean(pnl)),
        'median_contract_pnl_usd':float(np.median(pnl)),'profitable_days':sum(v>0 for v in daily.values()),'days':len(daily),
        'profitable_contracts':int(np.sum(pnl>0)),'losing_contracts':int(np.sum(pnl<0)),'zero_contracts':int(np.sum(pnl==0)),
        'no_quotes_contracts':sum(r['submitted']==0 for r in rows),'no_fills_contracts':sum(r['fills']==0 for r in rows),
        'submitted':submitted,'fills':sum(r['fills'] for r in rows),
        'cancel_pending_fill_lots':sum(r['cancel_pending_fill_lots'] for r in rows),'post_expiry_fill_lots':sum(r['post_expiry_fill_lots'] for r in rows),
        'weighted_markout_100ms_1s_5s':marks,'markout_quantity':q.tolist(),'daily_pnl_usd':daily,
        'chronological_independent_contract_pnl_max_drawdown_usd':float(np.max(np.maximum.accumulate(curve)-curve)),
        'profit_factor':gross_profit/gross_loss if gross_loss else None,
        'day_block_bootstrap_total_pnl_95pct_usd':np.quantile(draws,[.025,.975]).tolist(),
        'five_best_contracts_pnl_usd':float(np.sort(pnl)[-5:].sum()),
        'worst_day_pnl_usd':float(values.min()),'best_day_pnl_usd':float(values.max()),
        'pnl_without_best_day_usd':float(pnl.sum()-values.max()),
        'posthoc_operating_cost_usd_per_contract':{str(cost):float(pnl.sum()-cost*len(rows)) for cost in (.05,.10,.25,.50)},
        'gross_cash_change_usd':sum((r['cash_tick']-100000000)/1e6 for r in rows),
        'settlement_inventory_change_usd':sum((r['up_lots'] if r['winner']=='Up' else r['down_lots'])-25 for r in rows),
        'end_abs_net_lots_mean':float(np.mean([abs(r['up_lots']-r['down_lots']) for r in rows])),
        'operating_cost_break_even_usd_per_1000_submits':float(pnl.sum()*1000/submitted) if submitted else None}


def report():
    result={'frozen_manifest':json.loads((OUT/'frozen_manifest.json').read_text()),'windows':{}}
    for window,data,expected,profiles in [('reused',OLD,960,list(PROFILES)),('confirmation',NEW,672,CONFIRM_PROFILES)]:
        result['windows'][window]={}
        for p in profiles:
            rows=[json.loads(f.read_text()) for f in sorted((OUT/window/p).glob('*/summary.json'))]
            if len(rows)!=expected:raise ValueError('incomplete profile '+window+'/'+p)
            result['windows'][window][p]=summarize(rows)
        if window=='confirmation':
            coverage=[json.loads(f.read_text()) for f in (data/'normalized').glob('*/coverage.json')]
            if len(coverage)!=expected:raise ValueError('incomplete new-window data coverage')
            result['confirmation_data_coverage']={k:float(np.mean([r[k] for r in coverage])) for k in ('paired_usable_fraction','external_features_ready_fraction')}
            result['confirmation_hourly_coverage']=json.loads((data/'coverage_summary.json').read_text())
    result['completed_replays']=sum(r['contracts'] for w in result['windows'].values() for r in w.values())
    base.save(OUT/'results.json',result)
    lines=['# 固定 shock-entry 扩展测试','', '原模型 SHA256 与策略参数固定，不重训、不按新结果改阈值。',
        '4 月 21–30 日曾用于选择 shock，以下为复用窗口压力测试；5 月 1–7 日为未参与选择的新窗口确认。','',
        '| 窗口 | 假设配置 | 市场数 | PnL ($) | 盈利天数 | 1s markout (每股美分) | 撤单等待成交股数 |',
        '|---|---|---:|---:|---:|---:|---:|']
    for w,profiles in result['windows'].items():
        for p,r in profiles.items():
            mo=r['weighted_markout_100ms_1s_5s'][1]
            lines.append(f"| {w} | {p} | {r['contracts']} | {r['pnl_usd']:.2f} | {r['profitable_days']}/{r['days']} | {mo*100:.4f} | {r['cancel_pending_fill_lots']} |")
    lines+=['','基准假设：BTC 接收 20ms、发单 25ms、撤单 100ms、通知 50ms、队列为已知显示量 1 倍。',
        'cancel200/cancel500、btc100/btc250、submit100、notice200 分别只改变命名延迟；queue2 为两倍队列。',
        'joint 同时采用 BTC 100ms、发单 100ms、撤单 200ms、通知 200ms、队列 2 倍。',
        '', 'PnL 含残留库存真实结算，Maker 费设置为零、补贴与运营成本为零，未启用期末 Taker 减仓。',
        '各市场独立以 $125 初始资本（25 对库存与 $100 剩余现金）运行，不是复利资金曲线。',
        'PM receive 是 PMXT exporter 的测量值；BTC 与订单链路延迟是假设，公共成交队列无法替代真实 FIFO。',
        '逐日重采样仅有 7 或 10 个日块，统计区间只作描述，不证明长期稳定性。',
        '', '[机器汇总](results.json) · [冻结清单](frozen_manifest.json) · [新数据覆盖](../pmxt_shock_fixed_20260501_20260507/coverage_summary.json)',
        '[数据来源说明](../../docs/pmxt_btc15m_backtest.md) · [Binance 原始数据](https://github.com/binance/binance-public-data)','']
    (OUT/'README.md').write_text('\n'.join(lines));print(json.dumps(result['windows'],indent=2),flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('phase',choices=['freeze','prepare','reused','confirmation','report']);p.add_argument('--workers',type=int,default=20);a=p.parse_args()
    if a.phase=='freeze':freeze()
    elif a.phase=='prepare':prepare_confirmation(a.workers)
    elif a.phase=='report':report()
    else:run_suite(a.phase,a.workers)


if __name__=='__main__':main()

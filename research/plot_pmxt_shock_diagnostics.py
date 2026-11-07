#!/usr/bin/env python3
"""Export the frozen shock diagnostic figures from audited local aggregates."""
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT=Path('runs/pmxt_shock_fixed_tests_20261006/diagnostics')
plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False})


def save(fig,name):
    fig.savefig(ROOT/(name+'.png'),dpi=180,bbox_inches='tight')
    fig.savefig(ROOT/(name+'.svg'),bbox_inches='tight');plt.close(fig)


def main():
    results=json.loads((ROOT/'results.json').read_text());extra=json.loads((ROOT/'regime_comparison.json').read_text())
    colors=['#3366aa','#e29a31'];fig,axes=plt.subplots(3,2,figsize=(13,10),layout='constrained')
    for i,p in enumerate(['baseline','cancel200','joint']):
        daily=results['pnl']['confirmation'][p]['daily'];days=list(daily);x=np.arange(len(days))
        closed=np.array([daily[d]['fifo']['closed_paired_usd'] for d in days]);terminal=np.array([daily[d]['fifo']['terminal_directional_usd'] for d in days])
        ax=axes[i,0];pos=np.zeros(len(x));neg=pos.copy()
        for values,color,label in zip([closed,terminal],colors,['Closed / paired (FIFO)','Terminal directional']):
            ax.bar(x,values,bottom=np.where(values>=0,pos,neg),color=color,label=label)
            pos+=np.maximum(values,0);neg+=np.minimum(values,0)
        ax.scatter(x,closed+terminal,s=35,color='black',zorder=4,label='Total PnL')
        ax.axhline(0,color='gray',linewidth=.7);ax.set_title(p+' — PnL attribution');ax.set_ylabel('USD, before operating costs')
        ax.set_xticks(x,[d[5:] for d in days]);ax.grid(axis='y',alpha=.15)
        if i==0:ax.legend(fontsize=8)
        ax=axes[i,1];mo=np.array([daily[d]['markout_1s_cents'] for d in days])
        ax.bar(x,mo,color=np.where(mo>=0,colors[0],'#c74a4a'));ax.axhline(0,color='gray',linewidth=.7)
        ax.set_title(p+' — 1s fill markout');ax.set_ylabel('Cents / share, volume weighted')
        ax.set_xticks(x,[d[5:] for d in days]);ax.grid(axis='y',alpha=.15)
    fig.suptitle('Frozen shock-entry — confirmation May 1–7, 2026 (UTC)')
    save(fig,'daily_pnl_attribution_and_markout')
    fig,axes=plt.subplots(4,2,figsize=(12,13),layout='constrained')
    specs=[('abs_alpha',100,'|alpha| (cents)'),('btc_sigma_bps_per_sqrt_second',1,'BTC RV (bps / sqrt(second))'),
        ('tau_seconds',1,'Time to expiry (seconds)'),('abs_distance_proxy_bps',1,'Distance to Binance start proxy (bps)'),
        ('abs_distance_proxy_sigma',1,'Proxy distance / sigma sqrt(tau)'),('spread',100,'Average UP / DOWN spread (cents)'),
        ('anchor_distance_from_half',100,'|PM anchor - 0.5| (cents)')]
    for ax,(k,mult,label) in zip(axes.flat,specs):
        for w,color in zip(['reused','confirmation'],colors):
            a=results['state'][w]['metrics'][k];ax.plot([10,50,90,99],np.array(a['p10_p50_p90_p99'])*mult,'o-',color=color,label=w)
        ax.set_title(label);ax.set_xticks([10,50,90,99]);ax.set_xlabel('Percentile, ready 50ms grid');ax.grid(alpha=.15)
    axes[0,0].legend();ax=axes[-1,-1];x=np.arange(2)
    for j,w in enumerate(['reused','confirmation']):
        r=results['state'][w];ax.bar(x+(j-.5)*.35,[100*r['ready_regime_fraction']['SHOCK'],100*r['abs_alpha_ge_halt_fraction']],width=.35,color=colors[j],label=w)
    ax.set_xticks(x,['Eligible SHOCK','Large shock / HALT']);ax.set_ylabel('% of ready grid');ax.set_title('Shock occupancy');ax.legend()
    fig.suptitle('Baseline state distributions — Apr 21–30 vs May 1–7 (UTC)')
    save(fig,'state_quantile_comparison')
    fig,axes=plt.subplots(1,3,figsize=(14,4),layout='constrained');horizons=[250,1000,5000,10000];x=np.array(horizons)/1000
    for w,color in zip(['reused','confirmation'],colors):
        r=extra['episode_groups'][w]['eligible_shock']['horizons']
        for ax,k,scale in zip(axes,['alpha_same_sign','pm_signed_change','btc_signed_return_bps'],[100,100,1]):
            ax.plot(x,[scale*r[str(h)][k]['mean'] for h in horizons],'o-',color=color,label=w);ax.grid(alpha=.15)
    for ax,title in zip(axes,['Alpha remains same sign (%)','PM continuation (cents)','BTC continuation (bps)']):
        ax.set_title(title);ax.set_xlabel('Seconds after eligible shock onset');ax.axhline(0,color='gray',linewidth=.7)
    axes[0].legend();fig.suptitle('Signal persistence and price continuation — event direction at onset')
    save(fig,'shock_continuation_comparison')


if __name__=='__main__':main()

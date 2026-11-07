#!/usr/bin/env python3
"""Standalone plot for the single actual-fill shock-age experiment."""
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

ROOT=Path('runs/pmxt_shock_fixed_tests_20261006/fill_conditional')


def main():
    r=json.loads((ROOT/'results.json').read_text());x=np.arange(7)
    labels=['pre (-1s)','0–100ms','100–250ms','250–500ms','0.5–1s','1–2s','2–5s']
    fig,axes=plt.subplots(2,3,figsize=(15,8),layout='constrained')
    for w,color in [('reused','#3366aa'),('confirmation','#c74a4a')]:
        bins=r['windows'][w]['bins']
        values=[[100*b['fill_probability'] for b in bins]]+[[b['markout_cents'][h] for b in bins] for h in range(3)]
        values += [[b['spread_cents'] for b in bins],[b['fill_lots'] for b in bins]]
        for ax,a in zip(axes.flat,values):ax.plot(x,a,'o-',color=color,label=w)
    titles=['Fill probability (%)','100ms markout (cents / share)','1s markout (cents / share)',
            '5s markout (cents / share)','Visible token spread at fill (cents)','Actual filled shares']
    for ax,title in zip(axes.flat,titles):
        ax.set_title(title);ax.set_xticks(x,labels,rotation=30,ha='right');ax.grid(alpha=.15)
        ax.spines[['top','right']].set_visible(False)
    for ax in [axes[0,1],axes[0,2],axes[1,0]]:ax.axhline(0,color='gray',linewidth=.8)
    axes[0,0].legend()
    fig.suptitle('Frozen baseline: actual-fill conditional shock age — Apr 21–30 vs May 1–7, 2026 UTC')
    fig.savefig(ROOT/'fill_conditional.png',dpi=180,bbox_inches='tight')
    fig.savefig(ROOT/'fill_conditional.svg',bbox_inches='tight')


if __name__=='__main__':main()

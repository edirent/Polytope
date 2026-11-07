#!/usr/bin/env python3
"""Freeze explicit PUBLIC markets and same-condition binary PAPER artifacts. No order API."""
import argparse,json,hashlib,subprocess
from pathlib import Path
from urllib.request import urlopen,Request
from datetime import datetime,timezone
ROOT=Path(__file__).resolve().parents[1]
def fetch(url):
    with urlopen(Request(url,headers={'User-Agent':'Polytope-runtime-validation/0.2.0'}),timeout=20) as r:
        raw=r.read();return json.loads(raw),hashlib.sha256(raw).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--kalshi',required=True);p.add_argument('--polymarket-id',required=True);a=p.parse_args()
    out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=False)
    ku='https://external-api.kalshi.com/trade-api/v2/markets/'+a.kalshi
    pu='https://gamma-api.polymarket.com/markets/'+a.polymarket_id
    k,kh=fetch(ku);k=k['market'];m,ph=fetch(pu)
    outcomes=json.loads(m['outcomes']) if isinstance(m['outcomes'],str) else m['outcomes']
    tokens=json.loads(m['clobTokenIds']) if isinstance(m['clobTokenIds'],str) else m['clobTokenIds']
    if [x.lower() for x in outcomes]!=['yes','no'] or len(tokens)!=2 or not m.get('acceptingOrders') or m.get('closed'):
        raise SystemExit('Market is not an active supported YES/NO CTF market')
    if k.get('status')!='active':raise SystemExit('Kalshi market is not active')
    markets=[dict(venue='kalshi',raw=k),dict(venue='polymarket_international',raw=m)]
    (out/'markets.public.json').write_text(json.dumps(markets,indent=2)+'\n')
    condition=m['conditionId'];yes,no=tokens
    (out/'oracle_input.jsonl').write_text(json.dumps(dict(market_id=condition,event_id=str(m.get('events',[{}])[0].get('id','')),title=m['question'],description=m.get('description',''),outcomes=['YES','NO'],asset_ids=tokens,resolution_source='public CTF same-condition binary metadata; paper-only',end_time=m.get('endDate',''),fetched_at_ns=1,source='public Gamma'))+'\n')
    rule=dict(rule_id='same_condition_binary',type='ExactlyOne',variable_ids=[condition+':YES',condition+':NO'],approved=True,approved_by='structural_binary_paper_only_not_cross_venue_approval',approved_at_ns=1,source_rule_draft_id='public-same-condition-outcomes')
    (out/'rulebook.paper.json').write_text(json.dumps({'rules':[rule]},indent=2)+'\n')
    bundle=dict(bundle_id=1,required_true_mask=0,required_false_mask=0,invalid_mask=0,guaranteed_payout_tick=1000000,min_edge_tick=0,legs=[dict(market_id=condition,asset_id=t,side='Buy',quantity_lots=1,max_price_tick=999999) for t in tokens])
    (out/'bundles.paper.json').write_text(json.dumps({'bundles':[bundle]},indent=2)+'\n')
    subprocess.run([str(ROOT/'build/verify_oracle_workflow'),'--market-snapshot',str(out/'oracle_input.jsonl'),'--rulebook',str(out/'rulebook.paper.json'),'--candidate-bundles',str(out/'bundles.paper.json'),'--out',str(out/'oracle')],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
    base=json.loads((ROOT/'config/trading_runtime.replay.json').read_text())
    for mode in ('PAPER','SHADOW'):
        folder=out/mode.lower();cfg=dict(base,schema_version=2,mode=mode,input_format='live_ws',input=str(folder/'inputs.jsonl'),output_dir=str(folder),oracle_artifact=str(out/'oracle'),markets_file=str(out/'markets.public.json'),max_records=500000,duration_seconds=1800,queue_capacity=4096,max_book_age_ns=15_000_000_000,require_kalshi_auth=True,diagnostic_reconnect_after_seconds=120,allow_test_faults=False)
        (out/(mode.lower()+'.json')).write_text(json.dumps(cfg,indent=2)+'\n')
    (out/'selection_manifest.json').write_text(json.dumps(dict(observed_utc=datetime.now(timezone.utc).isoformat(),kalshi_market=a.kalshi,polymarket_market=condition,polymarket_id=a.polymarket_id,source_urls=[ku,pu],source_hashes=[kh,ph],cross_venue_pair_approval=False,payoff_scope='same-condition binary PAPER simulation only',fees_enabled=m.get('feesEnabled'),actual_exchange_receipts=False),indent=2)+'\n')
    print('Public validation inputs frozen at',out)
if __name__=='__main__':main()

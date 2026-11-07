#!/usr/bin/env python3
"""Bounded native V021 scenarios over immutable real recordings and labeled synthetic inputs."""
import argparse,copy,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MODELS=['optimistic','conservative','queue_aware','adverse_selection']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,v):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(v,indent=2)+'\n')
def configure(base,instruments):
    cfg=copy.deepcopy(base);assets=[i['asset_id'] for i in instruments]
    cfg.update(schema_version=3,strategy='multi_strategy',mode='REPLAY',replay_execution_policy='PAPER',record_strategy_frames=True,
        max_order_cost_tick=10000000,max_total_exposure_tick=50000000,max_market_exposure_tick=50000000,max_asset_inventory_lots=100,
        maker_tick_policy='PASSIVE_OUTWARD',max_records=200000,allow_test_faults=False,instruments=instruments,max_book_age_ns=15000000000)
    cfg['maker_execution']=dict(model='conservative',submit_latency_ns=50000000,cancel_latency_ns=100000000,ack_timeout_ns=1000000000,fee_per_lot_tick=0,queue_min_rest_ns=250000000)
    cfg['strategies']=[dict(strategy_id=k,strategy_instance_id=i,configuration_epoch=1,priority=n,budget_tick=40000000,callback_budget_ns=100000000,
        assets=assets,model={} if k=='combinatorial' else dict(base_quote_size_lots=5,min_half_spread_tick=1000,max_inventory_lots=100,max_inventory_skew_tick=10000,
            min_requote_interval_ns=500000000,quote_ttl_ns=2000000000)) for n,(k,i) in enumerate([('combinatorial','combo'),('market_making','maker')])]
    return cfg
def prepare(out,binary,seconds):
    out.mkdir(parents=True,exist_ok=True)
    prior=ROOT/'runs/runtime_v020/validation_release_20261106';base=json.loads((prior/'paper.json').read_text());markets=json.loads(Path(base['markets_file']).read_text())
    market=next(x['raw'] for x in markets if x['venue']=='polymarket_international');tokens=json.loads(market['clobTokenIds']) if isinstance(market['clobTokenIds'],str) else market['clobTokenIds']
    indices=json.loads(subprocess.check_output([str(ROOT/'build/runtime_maker_parity'),'--inspect-artifact',base['oracle_artifact']],text=True))
    from decimal import Decimal,ROUND_CEILING
    tick=int(Decimal(str(market.get('orderPriceMinTickSize','0.001')))*1000000);minimum=int(Decimal(str(market.get('orderMinSize',5))).to_integral_value(rounding=ROUND_CEILING))
    instruments=[dict(asset_id=a,asset_index=indices[a],market_id=market['conditionId'],market_index=0,venue='polymarket_international',account='PAPER',settlement_asset='USDC',tick_size_tick=tick,min_qty_lots=minimum,quantity_step_lots=1) for a in tokens]
    source=Path(base['input']);prefix=out/'inputs/real_prefix.jsonl';prefix.parent.mkdir(exist_ok=True);first=None;count=0;last=0
    with source.open() as src,prefix.open('x') as dest:
        for line in src:
            row=json.loads(line)['event'];stamp=row['recv_ns']
            if first is None:first=stamp
            if stamp>first+seconds*1000000000:break
            dest.write(line);count+=1;last=stamp
    save(out/'recording_lineage.json',dict(original_input=str(source),original_sha256=sha(source),derived_input=str(prefix),derived_sha256=sha(prefix),
        rows=count,recorded_availability_seconds=(last-first)/1e9,note='Unmodified hash-chain prefix; original recording remains immutable; not a new live observation',fees_enabled=market.get('feesEnabled')))
    real=configure(base,instruments);real.update(input=str(prefix),input_format='venue_journal',kill_switch_file=str(out/'KILL'))
    for model in MODELS:
        cfg=copy.deepcopy(real);cfg['maker_execution']['model']=model;cfg['output_dir']=str(out/'real_recorded'/model);save(out/'profiles'/('real_'+model+'.json'),cfg)
    synthetic_base=json.loads((ROOT/'config/trading_runtime.replay.json').read_text());synthetic_base['oracle_artifact']=str(ROOT/'tests/fixtures/runtime/oracle_artifact')
    sinstruments=[dict(asset_id=a,asset_index=n,market_id='m1',market_index=0,venue='polymarket_international',account='PAPER',settlement_asset='USDC',tick_size_tick=1000,min_qty_lots=1) for n,a in enumerate(['asset_yes','asset_no'])]
    synthetic=configure(synthetic_base,sinstruments);synthetic['maker_execution']['fee_per_lot_tick']=3;synthetic['strategies'][1]['model'].update(min_half_spread_tick=10000,max_inventory_skew_tick=0,min_requote_interval_ns=200000000)
    rows=[]
    for n in range(160):
        for side,asset in enumerate(['asset_yes','asset_no']):
            stamp=1000000000+n*200000000+side*1000000;mid=[500000,480000,520000,470000,530000][n%5] if side==0 else 500000
            row=dict(kind='packet',recv_ns=stamp,connection_id=1,payload=dict(event_type='book',market='m1',asset_id=asset,timestamp=str(stamp//1000000),hash=f'synthetic-{n}-{side}',
                bids=[dict(price=f'{(mid-10000)/1000000:.6f}',size='100')],asks=[dict(price=f'{(mid+10000)/1000000:.6f}',size='100')]))
            if n>0:row['public_trade']=dict(asset_id=asset,price_tick=mid-20000 if n%2==0 else mid+20000,quantity_lots=3,side='Sell' if n%2==0 else 'Buy')
            rows.append(row)
    path=out/'inputs/synthetic.jsonl';path.write_text(''.join(json.dumps(x)+'\n' for x in rows))
    synthetic.update(input=str(path),input_format='jsonl',kill_switch_file=str(out/'KILL'))
    for model in MODELS:
        cfg=copy.deepcopy(synthetic);cfg['maker_execution']['model']=model;cfg['output_dir']=str(out/'synthetic'/model);save(out/'profiles'/('synthetic_'+model+'.json'),cfg)
    # Bounded public-only live follow-up reuses the verified existing feed interface.
    live_meta=out/'inputs/live_markets.public.json';save(live_meta,[x for x in markets if x['venue']=='polymarket_international'])
    for mode in ['PAPER','SHADOW']:
        cfg=copy.deepcopy(real);folder=out/'live'/mode.lower();cfg.update(mode=mode,input_format='live_ws',input=str(folder/'inputs.jsonl'),output_dir=str(folder),markets_file=str(live_meta),
            duration_seconds=180,require_kalshi_auth=False,diagnostic_reconnect_after_seconds=60)
        save(out/'profiles'/('live_'+mode.lower()+'.json'),cfg)
    save(out/'validation_plan.json',dict(binary=str(binary),binary_sha256=sha(binary),models=MODELS,live_duration_seconds=180,live_scope='Public Polymarket only; no new Kalshi authentication claim',
        real_recording_seconds=seconds,private_account_reconciled=False,verified_live_realized_pnl=None))
    print(json.dumps(dict(prepared=str(out),real_rows=count)))
def execute(out,binary,profile):
    cfg=out/'profiles'/(profile+'.json');data=json.loads(cfg.read_text());folder=Path(data['output_dir'])
    if folder.exists():raise SystemExit('Output already exists; preserve evidence and choose a new profile')
    log=out/'logs'/(profile+'.log');log.parent.mkdir(exist_ok=True)
    with log.open('w') as stream:r=subprocess.run([str(binary),'--config',str(cfg)],cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,timeout=900)
    save(out/'logs'/(profile+'.result.json'),dict(profile=profile,returncode=r.returncode,binary_sha256=sha(binary),config_sha256=sha(cfg)))
    if r.returncode:raise SystemExit(r.returncode)
    if not profile.startswith('live_'):
        with (out/'logs'/(profile+'.parity.log')).open('w') as stream:
            done=subprocess.run([str(ROOT/'build/runtime_maker_parity'),str(cfg),str(folder/'events.jsonl'),str(folder/'strategy_parity.csv')],stdout=stream,stderr=subprocess.STDOUT,timeout=60)
        if done.returncode:raise SystemExit('Maker parity failed; inspect evidence')
    print(profile,'completed')
def prepare_live(out,market_file,label):
    """Explicit new market epoch; existing recordings/configurations are never overwritten."""
    m=json.loads(Path(market_file).read_text());folder=out/('live_selection_'+label);folder.mkdir(exist_ok=False)
    tokens=json.loads(m['clobTokenIds']);outcomes=json.loads(m['outcomes'])
    if not m.get('acceptingOrders') or m.get('closed') or m.get('feesEnabled') is not False or [v.lower() for v in outcomes]!=['yes','no']:
        raise RuntimeError('Active fee-free binary metadata required for this bounded profile')
    condition=m['conditionId'];save(folder/'markets.public.json',[dict(venue='polymarket_international',raw=m)])
    (folder/'markets.jsonl').write_text(json.dumps(dict(market_id=condition,event_id=str(m.get('events',[{}])[0].get('id','')),title=m['question'],description=m.get('description',''),outcomes=['YES','NO'],asset_ids=tokens,
        resolution_source='public same-condition binary paper-only',end_time=m.get('endDate',''),fetched_at_ns=1,source='public Gamma'))+'\n')
    save(folder/'rulebook.json',{'rules':[dict(rule_id='same_condition_binary',type='ExactlyOne',variable_ids=[condition+':YES',condition+':NO'],approved=True,
        approved_by='structural_binary_paper_only_not_cross_venue_approval',approved_at_ns=1,source_rule_draft_id='public-same-condition-outcomes')]})
    save(folder/'bundles.json',{'bundles':[dict(bundle_id=1,required_true_mask=0,required_false_mask=0,invalid_mask=0,guaranteed_payout_tick=1000000,min_edge_tick=0,
        legs=[dict(market_id=condition,asset_id=t,side='Buy',quantity_lots=1,max_price_tick=999999) for t in tokens])]})
    subprocess.run([str(ROOT/'build/verify_oracle_workflow'),'--market-snapshot',str(folder/'markets.jsonl'),'--rulebook',str(folder/'rulebook.json'),
        '--candidate-bundles',str(folder/'bundles.json'),'--out',str(folder/'oracle')],check=True,stdout=subprocess.DEVNULL)
    indices=json.loads(subprocess.check_output([str(ROOT/'build/runtime_maker_parity'),'--inspect-artifact',str(folder/'oracle')],text=True))
    from decimal import Decimal
    for mode in ['paper','shadow']:
        cfg=json.loads((out/'profiles'/('live_final_'+mode+'.json')).read_text());target=out/'experiments_final'/('live_'+label)/mode
        cfg.update(input=str(target/'inputs.jsonl'),output_dir=str(target),markets_file=str(folder/'markets.public.json'),oracle_artifact=str(folder/'oracle'))
        cfg['instruments']=[dict(asset_id=t,asset_index=indices[t],market_id=condition,market_index=0,venue='polymarket_international',account='PAPER',settlement_asset='USDC',
            tick_size_tick=int(Decimal(str(m['orderPriceMinTickSize']))*1000000),min_qty_lots=int(m['orderMinSize']),quantity_step_lots=1) for t in tokens]
        for strategy in cfg['strategies']:strategy['assets']=tokens;strategy['configuration_epoch']=2
        save(out/'profiles'/('live_'+label+'_'+mode+'.json'),cfg)
    save(folder/'selection_manifest.json',dict(market_id=m['id'],condition=condition,title=m['question'],source_url='https://gamma-api.polymarket.com/markets/'+str(m['id']),
        metadata_sha256=sha(folder/'markets.public.json'),fees_enabled=m['feesEnabled'],cross_venue_approval=False,paper_only=True))
    print('New bounded live epoch prepared:',label,m['id'])
def main():
    p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--binary',default='build/trading_runtime');p.add_argument('--prepare',action='store_true');p.add_argument('--recorded-seconds',type=int,default=600);p.add_argument('--profile');p.add_argument('--prepare-live');p.add_argument('--market-file');a=p.parse_args()
    out=Path(a.out).resolve();binary=Path(a.binary).resolve()
    if a.prepare_live:prepare_live(out,a.market_file,a.prepare_live)
    elif a.prepare:prepare(out,binary,a.recorded_seconds)
    elif a.profile:execute(out,binary,a.profile)
    else:p.error('--prepare or --profile required')
if __name__=='__main__':main()

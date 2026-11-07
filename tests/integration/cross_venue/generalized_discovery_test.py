"""Synthetic source-semantic regressions; no network or real approvals."""
import json,subprocess,sys,tempfile
from pathlib import Path
BINARY=Path(sys.argv[1]).resolve();ROOT=Path(__file__).resolve().parents[3]
OBS=1793952000000000000

def kmarket(name,threshold,time='12:00 UTC',index='CF Benchmarks',event='SYNTHETIC_PRICE'):
    return {'venue':'kalshi','raw':{'ticker':name,'event_ticker':event,'title':'SYNTHETIC Bitcoin price fixture','rules_primary':f'SYNTHETIC ONLY. If Bitcoin price from {index} is above {threshold} at {time} on November 7, 2026, this market resolves to Yes.','rules_secondary':'SYNTHETIC no actual approval','status':'active','market_type':'binary','floor_strike':threshold,'strike_type':'greater','close_time':'2026-11-07T13:00:00Z','open_interest_fp':'2000','volume_24h_fp':'2000','minimum_order_size_fp':'1.25','quantity_step_fp':'0.25','price_ranges':[{'start':'0','end':'1','step':'0.01'}],'_observed_wall_ns':OBS}}

with tempfile.TemporaryDirectory(prefix='polytope-v013-SYNTHETIC-') as directory:
    root=Path(directory);markets=[kmarket('SYN_A',100),kmarket('SYN_B',90),kmarket('SYN_WRONG_TIME',80,'13:00 UTC'),kmarket('SYN_WRONG_INDEX',70,index='Binance')]
    football_tail='This market refers only to the outcome within the first 90 minutes of regular play plus stoppage time.\nSYNTHETIC_ONLY identical source definition.'
    for name,question,conditional,cancel in [('SYN_HOME','Will Synthetic Home FC win?','If Synthetic Home FC wins, this market will resolve to "Yes".','No'),('SYN_DRAW','Will Synthetic Home FC vs. Synthetic Away FC end in a draw?','If the game ends in a draw, this market will resolve to "Yes".','Yes'),('SYN_AWAY','Will Synthetic Away FC win?','If Synthetic Away FC wins, this market will resolve to "Yes".','No')]:
        m=kmarket(name,1,event='SYNTHETIC_MATCH');m['raw'].pop('strike_type');m['raw'].pop('floor_strike');m['raw']['title']=question;m['raw']['rules_primary']='SYNTHETIC upcoming game November 7, 2026.\n'+conditional+'\nOtherwise No.\n'+f'If the game is canceled entirely, with no make-up game, this market will resolve to "{cancel}".\n'+football_tail;m['raw']['rules_secondary']='';markets.append(m)
    markets.append({'venue':'polymarket_international','raw':{'id':'SYN_UPDOWN','conditionId':'0x'+'d'*64,'question':'SYNTHETIC Bitcoin Up or Down','description':'SYNTHETIC ONLY Chainlink price comparison at 14:00 UTC; no actual relationship approved.','version':'v1','outcomes':['Up','Down'],'clobTokenIds':['111','222'],'active':True,'closed':False,'enableOrderBook':True,'acceptingOrders':True,'endDate':'2026-11-07T14:00:00Z','orderPriceMinTickSize':'.01','orderMinSize':5,'liquidityNum':2000,'volume24hr':2000,'_observed_wall_ns':OBS}})
    config={'schema':'cross-venue-research-v0.1','mode':'read_only_shadow','capital':'200000','venue_allocations':['100000','100000'],'allow_directional_binary_outcomes':True,'historical_captures':[],'generalized':{'max_legs':4,'max_candidate_groups':512,'max_per_type':40,'minimum_liquidity_proxy':1,'minimum_observation_horizon_sec':0,'max_observation_horizon_days':14}}
    (root/'config.json').write_text(json.dumps(config));(root/'metadata.json').write_text(json.dumps({'evidence_level':'SYNTHETIC_REGRESSION','markets':markets}))
    out=root/'output';subprocess.run([str(BINARY),'--phase','discover','--config',str(root/'config.json'),'--universe',str(root/'metadata.json'),'--out',str(out)],check=True,cwd=ROOT,stdout=subprocess.DEVNULL)
    proofs=[json.loads(line) for line in (out/'payoff_proofs.jsonl').open()]
    nested=[p for p in proofs if p['type']=='NESTED_THRESHOLD'];assert len(nested)==1
    assert {l['market']['market_id'] for l in nested[0]['legs']}=={'SYN_A','SYN_B'},'different timestamp/index silently normalized away'
    partitions=[p for p in proofs if p['type']=='EXHAUSTIVE_YES_BUNDLE' and len(p['legs'])==3 and {l['market']['market_id'] for l in p['legs']}=={'SYN_HOME','SYN_DRAW','SYN_AWAY'}];assert len(partitions)==1
    p=partitions[0];assert p['hypothetical_normal_minimum']==1000000 and p['justified_normal_minimum']==1000000
    cancellation=p['exceptional_payoffs'][0];assert cancellation['branch']=='CANCELLATION' and sorted(cancellation['payout_per_unit_micro'])==[0,0,1000000]
    assert 'CANCELLATION' not in p['missing_states'] and 'DISPUTE' in p['missing_states'] and not p['coverage_complete'] and p['approval_status']=='PENDING_HUMAN_REVIEW'
    universe=[json.loads(x) for x in (out/'market_universe.jsonl').open()];up=next(m for m in universe if m['venue']=='polymarket_international');assert up['yes']['instrument_id']=='111' and up['no']['instrument_id']=='222' and up['raw']['outcomes']==['Up','Down'] and up['protocol_supported']
    bad=root/'corrupt.jsonl';text=(out/'payoff_proofs.jsonl').read_text();bad.write_text(text.replace('SYNTHETIC no actual approval','CORRUPTED SOURCE',1))
    r=subprocess.run([str(BINARY),'--phase','replay','--config',str(root/'config.json'),'--relationships',str(bad),'--run-dir',str(root/'NO_REAL_CAPTURE'),'--out',str(root/'bad')],capture_output=True,text=True);assert r.returncode and 'hash mismatch' in r.stderr
    r=subprocess.run([str(BINARY),'--live-enabled','true'],capture_output=True,text=True);assert r.returncode and 'no order submission' in r.stderr
    print('SYNTHETIC discovery tests passed: compatible thresholds; timestamp/index counterexamples; completed-match partition with explicit cancellation and missing dispute; opt-in directional labels; artifact corruption; no-live option.')

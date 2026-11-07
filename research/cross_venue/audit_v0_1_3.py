"""Independent stdlib payoff, L2, fee, sizing, capital and provenance audit.
No networking, credentials, approval changes or trading access.
"""
import argparse,csv,hashlib,itertools,json,re
from collections import defaultdict
from decimal import Decimal
from pathlib import Path
U=1000000
BRANCHES={'TIE','VOID','CANCELLATION','POSTPONEMENT','EARLY_TERMINATION','DISPUTE','REPLACEMENT','FRACTIONAL_REFUND','SOURCE_DIVERGENCE','UNDEFINED'}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def micro(x):
    n=Decimal(str(x))*U;assert n==n.to_integral_value();return int(n)
def table(p):return [r for r in csv.DictReader(p.open()) if r.get('status')!='UNAVAILABLE']
def feasible(mask,constraints):
    for c in constraints:
        value=sum(coef*((mask>>i)&1) for i,coef in zip(c['variable_ids'],c['coefficients']));rhs=c['rhs']
        if not {'EQ':value==rhs,'LE':value<=rhs,'GE':value>=rhs}[c['operator']]:return False
    return True
def states(proof,field):
    return [(mask,[U*(((mask>>l['variable_id'])&1) if l['outcome_side']=='YES_OR_UP' else 1-((mask>>l['variable_id'])&1)) for l in proof['legs']]) for mask in range(1<<len(proof['variables'])) if feasible(mask,proof[field])]
def floor(payoffs,quantities):return min(sum(q*p//U for q,p in zip(quantities,row)) for row in payoffs)
def read_proofs(path):
    proofs={};normal_count=counter_count=0
    for line in path.read_text().splitlines():
        p=json.loads(line);body=re.sub(r',"artifact_sha256":"[a-f0-9]{64}"}$','}',line);assert body!=line and hashlib.sha256(body.encode()).hexdigest()==p['artifact_sha256']
        assert 2<=len(p['legs'])<=4 and 1<=len(p['variables'])<=4;assert len({(x['instrument']['venue'],x['instrument']['market_id'],x['instrument']['instrument_id']) for x in p['legs']})==len(p['legs'])
        for leg in p['legs']:
            m=leg['market'];raw=m['raw'];rules=raw.get('rules_primary','')+'\n'+raw.get('rules_secondary','') if m['venue']=='kalshi' else raw['description'];assert hashlib.sha256(rules.encode()).hexdigest()==m['rules_hash']==leg['rules_hash']
            expected=m['yes'] if leg['outcome_side']=='YES_OR_UP' else m['no'];assert expected==leg['instrument']
            if m['venue']=='kalshi':assert expected['instrument_id']==('YES' if leg['outcome_side']=='YES_OR_UP' else 'NO')
            else:
                outcomes=raw['outcomes'];tokens=raw['clobTokenIds'];outcomes=json.loads(outcomes) if isinstance(outcomes,str) else outcomes;tokens=json.loads(tokens) if isinstance(tokens,str) else tokens;aliases={'yes','up'} if leg['outcome_side']=='YES_OR_UP' else {'no','down'};allowed={outcomes[i].lower():tokens[i] for i in range(len(outcomes))};assert any(allowed.get(name)==expected['instrument_id'] for name in aliases)
        normal=states(p,'assumed_constraints');justified=states(p,'justified_normal_constraints');assert normal and justified
        assert [(s['mask'],s['payout_per_unit_micro']) for s in p['normal_payoff_states']]==normal
        assert [(s['mask'],s['payout_per_unit_micro']) for s in p['justified_normal_states']]==justified
        nominal=floor([x[1] for x in normal],[U]*len(p['legs']));assert nominal==p['hypothetical_normal_minimum'];assert floor([x[1] for x in justified],[U]*len(p['legs']))==p['justified_normal_minimum']
        normal_count+=nominal>0;counter_count+=nominal==0
        assert p['mathematical_status']==('MATHEMATICALLY_VALID_PENDING_RULE_REVIEW' if nominal>0 else 'COUNTEREXAMPLE_FOUND')
        complete=list(p['justified_normal_states']);known={}
        for branch in p.get('exceptional_payoffs',[]):
            assert branch['branch'] in BRANCHES and branch['branch'] not in known;known[branch['branch']]=branch
            if branch['evidence_verified'] and branch['branch']=='CANCELLATION':
                values=[];source=''
                for l in p['legs']:
                    clause=re.search(r'If the game is canceled entirely, with no make-up game, this market will resolve (?:to )?"(Yes|No)"\.',l['market']['rules']);assert clause
                    yes=U if clause[1]=='Yes' else 0;values.append(yes if l['outcome_side']=='YES_OR_UP' else U-yes);source+='64:'+l['rules_hash']
                assert values==branch['payout_per_unit_micro'];assert hashlib.sha256(source.encode()).hexdigest()==branch['source_hash']
        assert set(p['missing_states'])==BRANCHES-set(known);assert not p['coverage_complete'];assert p['approval_status']=='PENDING_HUMAN_REVIEW'
        full=p['conservative_full_states'];assert [(x['mask'],x['payout_per_unit_micro']) for x in full[:len(justified)]]==justified
        for x in full[len(justified):]:
            name=x['id']
            if x['uncertainty_envelope']:assert x['payout_per_unit_micro']==[0]*len(p['legs']) and any(name.startswith(b+'_') for b in p['missing_states'])
            else:assert name in known and x['payout_per_unit_micro']==known[name]['payout_per_unit_micro']
        assert floor([x['payout_per_unit_micro'] for x in full],[U]*len(p['legs']))==p['conservative_lower_envelope_minimum']
        proofs[p['relationship_id']]=p
    return proofs,{'proof_artifacts':len(proofs),'conditional_normal_hypotheses_positive':normal_count,'normal_counterexamples':counter_count,'complete_contractual_proofs':0}
def cost_and_fee(asks,q,rate,kind):
    remaining=q;cost=fees=0;fragment_notional_ceilings=0
    for price,available in asks:
        take=min(remaining,available);cost+=take*price;remaining-=take
        if take:
            numerator=take*rate*price*(U-price);denominator=U**3;part=(numerator+denominator-1)//denominator
            if kind=='quadratic_cent_ceiling':
                notional=(take*price+U-1)//U;fragment_notional_ceilings+=notional;fees+=((notional+part+9999)//10000)*10000-notional
            else:fees+=((part+9)//10)*10
    assert remaining==0,'insufficient independently observed depth'
    if kind=='quadratic_cent_ceiling':fees+=fragment_notional_ceilings-(cost+U-1)//U
    return (cost+U-1)//U,fees

def audit(proofs_path,source,analysis,repeat=None):
    proofs,proof_stats=read_proofs(proofs_path);manifest=json.loads((source/'capture_manifest.json').read_text());raw=[];rawhash=defaultdict(list)
    for entry in manifest['raw_inventory']:
        name=entry['file'];assert Path(name).name==name;p=source/name;assert sha(p)==entry['sha256'];offset=0
        for line in p.read_bytes().splitlines(keepends=True):
            r=json.loads(line);assert r['raw_file']==name and r['raw_log_offset']==offset;offset+=len(line);assert hashlib.sha256(r['raw_payload'].encode()).hexdigest()==r['raw_hash'];assert not any(x in r['raw_payload'] for x in ['PRIVATE KEY-----','KALSHI-ACCESS-SIGNATURE','KALSHI-ACCESS-KEY']);raw.append(r);rawhash[r['raw_hash']].append(r)
    raw.sort(key=lambda r:r['record_sequence']);assert [r['record_sequence'] for r in raw]==list(range(1,len(raw)+1))
    if manifest.get('processing_timing_file'):
        p=source/manifest['processing_timing_file'];assert sha(p)==manifest['processing_timing_sha256'];timing=[json.loads(line) for line in p.open()];assert len(timing)==len(raw)
        for r,t in zip(raw,timing):assert t['record_sequence']==r['record_sequence'] and t['raw_hash']==r['raw_hash'];assert t['processing_completed_monotonic_ns']>=r['availability_monotonic_ns'];r['availability_monotonic_ns']=t['processing_completed_monotonic_ns']
    assert all(a['availability_monotonic_ns']<=b['availability_monotonic_ns'] for a,b in zip(raw,raw[1:]))
    selected=json.loads((source/'selected_markets.json').read_text());markets={(m['venue'],m['market_id']):m for m in selected};tokens={}
    for m in selected:
        if m['venue']=='polymarket_international':
            for side in ['yes','no']:tokens[m[side]['instrument_id']]=(m['venue'],m['market_id'],m[side]['instrument_id'])
    kbooks={};pbooks={};source_clocks={};sequences={};counts=defaultdict(int);transport={};epochs={}
    def update(side,p,q):
        p,q=micro(p),micro(q);assert 0<=p<=U and q>=0
        if q:side[p]=q
        else:side.pop(p,None)
    def consume(r):
        if r['transport']=='event':
            if r['classification']=='connection_open':transport[r['venue']]=True;epochs[r['venue']]=r['connection_epoch']
            elif r['classification'] in ['disconnect','connection_error','authentication_error','rebuild_requested','validation_reconnect','timeout','gap']:transport[r['venue']]=False
        if r['transport']!='ws' or r['classification']!='market_data':return
        try:packet=json.loads(r['raw_payload'])
        except ValueError:return
        if r['venue']=='kalshi':
            if not isinstance(packet,dict) or packet.get('type') not in ['orderbook_snapshot','orderbook_delta']:return
            counts[packet['type']]+=1;stream=(r['connection_epoch'],packet['sid']);seq=packet['seq']
            if stream in sequences:assert seq==sequences[stream]+1
            else:assert packet['type']=='orderbook_snapshot'
            sequences[stream]=seq;body=packet['msg'];key=body['market_ticker'];epoch=(r['connection_epoch'],key);clock=body.get('clock');assert clock and clock>=source_clocks.get(epoch,0);source_clocks[epoch]=clock
            if packet['type']=='orderbook_snapshot':
                kbooks[key]={'yes':{},'no':{}}
                for side in ['yes','no']:
                    for p,q in body.get(side+'_dollars_fp',[]):update(kbooks[key][side],p,q)
            else:
                assert key in kbooks;book=kbooks[key][body['side']];p=micro(body['price_dollars']);q=book.get(p,0)+micro(body['delta_fp']);assert q>=0
                if q:book[p]=q
                else:book.pop(p,None)
        else:
            for packet in packet if isinstance(packet,list) else [packet]:
                if packet.get('event_type')=='book' and packet.get('asset_id') in tokens:
                    key=tokens[packet['asset_id']];pbooks[key]={'bids':{},'asks':{}}
                    for side in ['bids','asks']:
                        for x in packet.get(side,[]):update(pbooks[key][side],x['price'],x['size'])
                elif packet.get('event_type')=='price_change':
                    for x in packet.get('price_changes',[]):
                        if x.get('asset_id') in tokens:
                            key=tokens[x['asset_id']];pbooks.setdefault(key,{'bids':{},'asks':{}});update(pbooks[key]['bids' if x['side']=='BUY' else 'asks'],x['price'],x['size'])
    costs=table(analysis/'portfolio_costs.csv');portfolios=table(analysis/'portfolio_candidates.csv');byportfolio=defaultdict(list);grouped=defaultdict(list)
    for row in costs:grouped[int(row['decision_ns'])].append(row);byportfolio[row['portfolio_id']].append(row)
    ri=0;walks=0
    for t,observed in sorted(grouped.items()):
        while ri<len(raw) and raw[ri]['availability_monotonic_ns']<=t:consume(raw[ri]);ri+=1
        for row in observed:
            key=json.loads(row['instrument']);venue,market,token=key['venue'],key['market_id'],key['instrument_id'];m=markets[(venue,market)];q=int(row['quantity_micro']);assert q>=0;assert transport.get(venue) and int(row['epoch'])==epochs[venue]
            if venue=='kalshi':actual=sorted((U-p,n) for p,n in kbooks[market]['no' if token=='YES' else 'yes'].items())[:64]
            else:actual=sorted(pbooks[(venue,market,token)]['asks'].items())[:64]
            visible=[(x['price_micro'],x['quantity_micro']) for x in json.loads(row['visible_asks'])];assert actual==visible,'native/independent real book differs';assert sum(n for p,n in actual)==int(row['observed_depth_micro']);assert q<=sum(n for p,n in actual)
            assert all(0<=p<=U and n>0 for p,n in actual)
            minimum,step,tick=int(row['minimum_micro']),int(row['step_micro']),int(row['tick_micro']);assert not q or minimum<=0 or q>=minimum;assert not q or step<=0 or q%step==0;assert tick>0 and all(p%tick==0 for p,n in actual)
            f=m['fee_metadata'];rate=micro(f['coefficient']) if venue=='kalshi' else (micro(f.get('rate',0)) if f.get('enabled') else 0);assert rate==int(row['fee_rate_micro']);c,fee=cost_and_fee(actual,q,rate,row['fee_schedule_type']);assert c==micro(row['acquisition_cost']) and fee==micro(row['fee']);walks+=1
            assert int(row['fee_observed_wall_ns'])<=int(row['decision_wall_ns'])<int(row['fee_expires_wall_ns'])
            recv,authority,changed,live=[int(row[k]) for k in ['book_recv_ns','authority_ns','last_change_ns','liveness_ns']];assert 0<recv<=t and 0<authority<=t and 0<live<=t and changed<=t;assert t-live<=30*10**9 and t-authority<=45*10**9;assert t-changed<=15*10**9 or authority>=changed
            assert any(r['recv_monotonic_ns']==recv and r['raw_log_offset']==int(row['raw_offset']) and r['availability_monotonic_ns']<=t for r in rawhash[row['raw_hash']]),'future or absent raw evidence'
    cfg=json.loads((analysis/'analysis_config.json').read_text())['generalized'];wallets=[micro(x) for x in cfg['wallets']];capital=micro(cfg['capital']);risk=micro(cfg['max_unhedged_cost']);maximum=cfg['max_quantity_micro'];forward=micro(cfg['forward_budget_per_share']);conversion=micro(cfg['conversion_budget_per_share']);cached={};optimized=0
    for row in portfolios:
        p=proofs[row['relationship_id']];assert row['artifact_sha256']==p['artifact_sha256'];q=json.loads(row['quantities_micro']);g=json.loads(row['quantity_grid_micro']);legs=sorted(byportfolio[row['portfolio_id']],key=lambda x:int(x['leg_index']));assert len(q)==len(g)==len(legs)==len(p['legs']);assert sum(n>0 for n in q)>=2
        c=sum(micro(x['acquisition_cost']) for x in legs);f=sum(micro(x['fee']) for x in legs);b=sum(micro(x['budget']) for x in legs);assert c==micro(row['acquisition_cost']) and f==micro(row['fees']) and b==micro(row['budgets'])
        pay=[x['payout_per_unit_micro'] for x in p['normal_payoff_states']];full=[x['payout_per_unit_micro'] for x in p['conservative_full_states']];nominal=floor(pay,q);safe=floor(full,q);assert nominal==micro(row['nominal_hypothesis_floor']) and safe==micro(row['conservative_uncertainty_lower_bound'])
        assert micro(row['optimizer_objective_including_abstention'])==max(0,micro(row['budget_adjusted_margin']))
        assert nominal-c==micro(row['gross_margin']) and nominal-c-f==micro(row['fee_adjusted_margin']) and nominal-c-f-b==micro(row['budget_adjusted_margin']);required=[0,0]
        pointsets=[]
        for i,leg in enumerate(legs):
            key=json.loads(leg['instrument']);assert key==p['legs'][i]['instrument'];assert int(leg['quantity_micro'])==q[i];v=0 if key['venue']=='kalshi' else 1;required[v]+=micro(leg['acquisition_cost'])+micro(leg['fee'])+micro(leg['budget']);asks=[(x['price_micro'],x['quantity_micro']) for x in json.loads(leg['visible_asks'])];rate=int(leg['fee_rate_micro']);points=[]
            for n in g[i]:
                assert 0<=n<=min(maximum,int(leg['observed_depth_micro']));assert not n or int(leg['minimum_micro'])<=0 or n>=int(leg['minimum_micro']);assert not n or int(leg['step_micro'])<=0 or n%int(leg['step_micro'])==0
                cost,fee=cost_and_fee(asks,n,rate,leg['fee_schedule_type']);budget=(n*forward+U-1)//U+((n*conversion+U-1)//U if p['venue_scope']=='CROSS_VENUE' else 0);points.append((n,cost,fee,budget,v))
                if n==q[i]:assert budget==micro(leg['budget'])
            assert q[i] in g[i];pointsets.append(points)
        assert required==[micro(row['wallet_kalshi']),micro(row['wallet_polymarket'])] and all(a<=b for a,b in zip(required,wallets)) and sum(required)<=capital and c+f<=risk
        cachekey=(p['relationship_id'],tuple(tuple(x) for x in pointsets))
        if cachekey not in cached:
            best=None;evaluated=0;gross_max=fee_max=None;gross_q=fee_q=None
            for choice in itertools.product(*pointsets):
                spent=[0,0];cost=sum(x[1] for x in choice);fee=sum(x[2] for x in choice);budget=sum(x[3] for x in choice)
                for n,cost_i,fee_i,budget_i,v in choice:spent[v]+=cost_i+fee_i+budget_i
                if any(a>b for a,b in zip(spent,wallets)) or sum(spent)>capital or cost+fee>risk:continue
                evaluated+=1
                if sum(x[0]>0 for x in choice)<2:continue
                quantities=[x[0] for x in choice];gross=floor(pay,quantities)-cost;afterfee=gross-fee;net=afterfee-budget;objective=(net,-cost)
                if gross_max is None or gross>gross_max:gross_max=gross;gross_q=quantities
                if fee_max is None or afterfee>fee_max:fee_max=afterfee;fee_q=quantities
                if best is None or objective>best:best=objective
            cached[cachekey]=(best,evaluated,gross_max,fee_max,gross_q,fee_q);optimized+=1
        objective,evaluated,gross_max,fee_max,gross_q,fee_q=cached[cachekey];assert micro(row['budget_adjusted_margin'])<=objective[0]
        if row['optimality'].startswith('EXACT_'):
            assert (micro(row['budget_adjusted_margin']),-c)==objective and int(row['quantity_combinations_evaluated'])==evaluated
            assert micro(row['max_feasible_gross_margin'])==gross_max and micro(row['max_feasible_fee_margin'])==fee_max
            assert json.loads(row['gross_witness_quantities_micro'])==gross_q and json.loads(row['fee_witness_quantities_micro'])==fee_q
        assert row['any_gross_positive']==str(micro(row['max_feasible_gross_margin'])>0).lower() and row['any_fee_positive']==str(micro(row['max_feasible_fee_margin'])>0).lower()
        assert micro(row['optimistic_box_relaxation_upper_bound'])>=max(objective[0],0)
        assert row['mode']=='UNAPPROVED_CONDITIONAL_PAYOFF_DIAGNOSTIC' and row['approval_status']=='PENDING_HUMAN_REVIEW' and row['strict_simulation_eligible']=='false' and not row['strict_pnl'] and not row['contractual_payout_floor'] and not row['verified_live_realized_pnl']
    primary={r['portfolio_id']:r for r in portfolios}
    for filename,field,witness in [('fee_adjusted_opportunities','max_feasible_fee_margin','fee'),('gross_opportunities','max_feasible_gross_margin','gross')]:
        positives=table(analysis/(filename+'.csv'));assert len(positives)==sum(micro(r[field])>0 for r in portfolios)
        for row in positives:
            original=primary[row['source_evidence_portfolio_id']];p=proofs[row['relationship_id']];q=json.loads(row['quantities_micro']);assert q==json.loads(original[witness+'_witness_quantities_micro']);legs=sorted(byportfolio[original['portfolio_id']],key=lambda x:int(x['leg_index']));c=f=b=0;spent=[0,0]
            for n,leg in zip(q,legs):
                cost,fee=cost_and_fee([(x['price_micro'],x['quantity_micro']) for x in json.loads(leg['visible_asks'])],n,int(leg['fee_rate_micro']),leg['fee_schedule_type']);budget=(n*forward+U-1)//U+((n*conversion+U-1)//U if p['venue_scope']=='CROSS_VENUE' else 0);c+=cost;f+=fee;b+=budget;spent[0 if json.loads(leg['instrument'])['venue']=='kalshi' else 1]+=cost+fee+budget
            nominal=floor([s['payout_per_unit_micro'] for s in p['normal_payoff_states']],q)
            assert (nominal-c-f if witness=='fee' else nominal-c)==micro(original[field])>0
            assert c==micro(row['acquisition_cost']) and f==micro(row['fees']) and b==micro(row['budgets']) and nominal-c-f-b==micro(row['budget_adjusted_margin'])
            assert nominal-c==micro(row['gross_margin']) and nominal-c-f==micro(row['fee_adjusted_margin'])
            assert spent==[micro(row['wallet_kalshi']),micro(row['wallet_polymarket'])] and not row['strict_pnl']
    # Independent second raw pass prices each future fixed-size scenario.
    kbooks.clear();pbooks.clear();source_clocks.clear();sequences.clear();transport.clear();epochs.clear();ri=0;latency_checks=0
    latency_rows=table(analysis/'latency_sensitivity.csv')
    for row in sorted(latency_rows,key=lambda x:int(x['execution_ns'])):
        decision,execution=int(row['decision_ns']),int(row['execution_ns']);assert execution-decision==int(row['latency_ms'])*1000000
        original=primary[row['portfolio_id']];p=proofs[row['relationship_id']];q=json.loads(row['quantities_micro'])
        witness={'NET_OPTIMIZER':'quantities_micro','GROSS_WITNESS':'gross_witness_quantities_micro','FEE_WITNESS':'fee_witness_quantities_micro'}[row['comparison_basis']];assert q==json.loads(original[witness])
        while ri<len(raw) and raw[ri]['availability_monotonic_ns']<=execution:consume(raw[ri]);ri+=1
        if row['status']=='CONDITIONAL_PRICE_STRESS':
            assert execution<=manifest['end_monotonic_ns'];assert Decimal(row['original_net'])-Decimal(row['later_net'])==Decimal(row['cost_deterioration'])
            legs=sorted(byportfolio[row['portfolio_id']],key=lambda x:int(x['leg_index']));c=f=b=0
            for n,leg in zip(q,legs):
                key=json.loads(leg['instrument']);v,market,token=key['venue'],key['market_id'],key['instrument_id'];assert transport.get(v)
                asks=sorted((U-price,size) for price,size in kbooks[market]['no' if token=='YES' else 'yes'].items())[:64] if v=='kalshi' else sorted(pbooks[(v,market,token)]['asks'].items())[:64]
                cost,fee=cost_and_fee(asks,n,int(leg['fee_rate_micro']),leg['fee_schedule_type']);c+=cost;f+=fee;b+=(n*forward+U-1)//U+((n*conversion+U-1)//U if p['venue_scope']=='CROSS_VENUE' else 0)
            nominal=floor([s['payout_per_unit_micro'] for s in p['normal_payoff_states']],q)
            assert nominal-c==micro(row['later_gross']) and nominal-c-f==micro(row['later_fee_adjusted']) and nominal-c-f-b==micro(row['later_net']);latency_checks+=1
            for e in json.loads(row['future_book_evidence']):assert e['recv_ns']<=e['frame_available_ns']<=execution and any(r['recv_monotonic_ns']==e['recv_ns'] and r['availability_monotonic_ns']<=execution for r in rawhash[e['raw_hash']])
        else:assert not row['later_net'] and not row['later_gross']
        assert not row['fill_probability'] and not row['simulated_trades']
    for row in table(analysis/'maker_feasibility.csv'):
        assert not row['maker_fill_probability'] and not row['maker_realized_pnl'] and row['order_eligible']=='false'
        if row.get('maker_quote_feasible')=='true':
            quote=micro(row['conditional_post_only_quote']);assert 0<quote<micro(row['best_ask']) and quote<=int(row['conditional_ceiling_micro'])
            if row['maker_quote_competitive']=='true':assert quote>=micro(row['best_bid'])
    grouped_episodes=defaultdict(list)
    for row in table(analysis/'opportunity_episodes.csv'):
        a,b=int(row['start_ns']),int(row['end_ns']);assert b>=a;assert abs(Decimal(row['duration_sec'])-Decimal(b-a)/10**9)<Decimal('.00000001');assert not row['strict_pnl'];grouped_episodes[(row['relationship_id'],row['basis'])].append((a,b))
    for intervals in grouped_episodes.values():
        s=sorted(intervals);assert all(a[1]<=b[0] for a,b in zip(s,s[1:])), 'duplicate overlapping episodes for same portfolio'
    episode_rows=table(analysis/'opportunity_episodes.csv')
    for basis,margin,quantity in [('GROSS','max_feasible_gross_margin','gross_witness_quantities_micro'),('FEE_ADJUSTED','max_feasible_fee_margin','fee_witness_quantities_micro'),('BUDGET_ADJUSTED','budget_adjusted_margin','quantities_micro')]:
        positive=[r for r in portfolios if micro(r[margin])>0];counts=defaultdict(list)
        for r in positive:
            matches=[i for i,e in enumerate(episode_rows) if e['relationship_id']==r['relationship_id'] and e['basis']==basis and e['quantity_vector_micro']==r[quantity] and int(e['start_ns'])<=int(r['decision_ns'])<int(e['end_ns'])];assert len(matches)==1;counts[matches[0]].append(r)
        for i,e in enumerate(episode_rows):
            if e['basis']==basis:assert len(counts[i])==int(e['distinct_observations']) and max(micro(r[margin]) for r in counts[i])==micro(e['peak_conditional_margin'])
    for row in table(analysis/'execution_risk.csv'):
        p=proofs[row['relationship_id']];q=json.loads(row['hypothetical_inventory_micro']);assert all(x>=0 for x in q);assert floor([s['payout_per_unit_micro'] for s in p['normal_payoff_states']],q)==micro(row['conditional_normal_payout_floor']);assert not row['actual_fills'] and not row['strict_pnl']
    summary=json.loads((analysis/'economic_summary.json').read_text());assert summary['raw_records_verified']==len(raw) and summary['approved_relationships']==0 and summary['strict_simulated_pnl'] is None and summary['verified_live_realized_pnl'] is None and summary['live_orders_sent']==0
    assert summary['priced_portfolio_states']==len(portfolios);assert summary['conditional_gross_positive']==sum(micro(x['max_feasible_gross_margin'])>0 for x in portfolios);assert summary['conditional_fee_positive']==sum(micro(x['max_feasible_fee_margin'])>0 for x in portfolios);assert summary['conditional_budget_positive']==sum(micro(x['budget_adjusted_margin'])>0 for x in portfolios)
    deterministic=0
    if repeat:
        for p in analysis.iterdir():
            if p.is_file():assert p.read_bytes()==(repeat/p.name).read_bytes(),p.name;deterministic+=1
    return {'status':'INDEPENDENT_GENERALIZED_ECONOMIC_AUDIT_PASSED',**proof_stats,'raw_records_verified':len(raw),'independent_all_instrument_L2_and_fee_checks':walks,'portfolio_states_verified':len(portfolios),'independent_future_fixed_quantity_cost_checks':latency_checks,'distinct_discrete_optima_independently_enumerated':optimized,'deterministic_files_compared':deterministic,'strict_approved_portfolios':0,'live_orders_sent':0,'verified_live_realized_pnl':None,'scope':'All reported legs: raw WS books, fees, sizes, cash, proof states, bounded-grid optima and future cutoffs. Conditional mathematical proofs are not complete contractual guarantees.'}
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--proofs',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--analysis',type=Path,required=True);p.add_argument('--repeat',type=Path);p.add_argument('--out',type=Path);a=p.parse_args();result=audit(a.proofs,a.source,a.analysis,a.repeat);text=json.dumps(result,indent=2)+'\n'
    if a.out:a.out.write_text(text)
    print(text)

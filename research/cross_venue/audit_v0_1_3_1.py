"""Independent validation of selection, quantity breakpoints and allocations.
Reuses V0.1.3 independent raw/proof/fee audit; no exchange or credential access.
"""
import csv,hashlib,itertools,json
from collections import Counter,defaultdict
from decimal import Decimal
from pathlib import Path
from audit_v0_1_3 import audit as base_audit,cost_and_fee,floor,micro,U,table,read_proofs

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def quantity_audit(proofs_file,analysis):
    proofs,_=read_proofs(proofs_file);cfg=json.loads((analysis/'analysis_config.json').read_text())['generalized'];forward=micro(cfg['forward_budget_per_share']);conversion=micro(cfg['conversion_budget_per_share']);maximum=cfg['max_quantity_micro'];wallets=[micro(n) for n in cfg['wallets']];capital=micro(cfg['capital']);risk=micro(cfg['max_unhedged_cost'])
    costs=defaultdict(list)
    for r in table(analysis/'portfolio_costs.csv'):costs[r['portfolio_id']].append(r)
    results=table(analysis/'quantity_breakpoint_analysis.csv');checked=0;bounds=0;valid_vectors=0
    for r in results:
        maximum=int(r.get('verification_max_quantity_micro') or cfg['max_quantity_micro']);p=proofs[r['relationship_id']];legs=sorted(costs[r['portfolio_id']],key=lambda l:int(l['leg_index']));asks=[[(x['price_micro'],x['quantity_micro']) for x in json.loads(l['visible_asks'])] for l in legs];pay=[s['payout_per_unit_micro'] for s in p['normal_payoff_states']];g=f=n=None;best_cost=None;byactive={};best_q=None
        for q in json.loads(r['quantity_vectors_micro']):
            assert len(q)==len(legs) and all(v>=0 for v in q)
            if sum(v>0 for v in q)<2:continue
            if any(v>min(maximum,int(l['observed_depth_micro'])) or (v and int(l['minimum_micro'])>0 and v<int(l['minimum_micro'])) or (v and int(l['step_micro'])>0 and v%int(l['step_micro'])) for v,l in zip(q,legs)):continue
            c=fees=b=0;spent=[0,0]
            for v,l,a in zip(q,legs,asks):
                cost,fee=cost_and_fee(a,v,int(l['fee_rate_micro']),l['fee_schedule_type']);budget=(v*forward+U-1)//U+((v*conversion+U-1)//U if p['venue_scope']=='CROSS_VENUE' else 0);c+=cost;fees+=fee;b+=budget;spent[0 if json.loads(l['instrument'])['venue']=='kalshi' else 1]+=cost+fee+budget
            if sum(spent)>capital or any(x>y for x,y in zip(spent,wallets)) or c+fees>risk:continue
            valid_vectors+=1;nominal=floor(pay,q);gross=nominal-c;fee=gross-fees;net=fee-b;active=sum(v>0 for v in q);byactive[active]=max(byactive.get(active,net),net)
            g=gross if g is None else max(g,gross);f=fee if f is None else max(f,fee)
            if n is None or (net,-c)>(n,-best_cost):n=net;best_cost=c;best_q=q
        assert n is not None
        assert n==micro(r['best_nonzero_net']) and max(0,n)==micro(r['best_zero_or_nonzero_net']);assert max(g,micro(r['baseline_max_gross']))==micro(r['max_gross_margin']) and max(f,micro(r['baseline_max_fee']))==micro(r['max_fee_margin'])
        assert len(json.loads(r['quantity_vectors_micro']))==int(r['tested_quantity_vectors'])
        assert r['new_budget_positive']==str(micro(r['baseline_best_nonzero_net'])<=0<n).lower()
        for active in [2,3,4]:
            field={2:'best_two_active_net',3:'best_three_active_net',4:'best_four_active_net'}[active]
            assert (micro(r[field]) if r[field] else None)==byactive.get(active)
        if r['normal_floor_is_minimum_of_two_quantities']=='true':
            assert len(legs)==2 and (U,0) in map(tuple,pay) and (0,U) in map(tuple,pay) and all(v in [(U,0),(0,U),(U,U)] for v in map(tuple,pay))
            maximum_common=min(maximum,*(sum(q for _,q in a) for a in asks));breaks={0,maximum_common}
            for a in asks:
                cumulative=0
                for _,q in a:
                    cumulative+=q
                    if cumulative<=maximum_common:breaks.add(cumulative)
            assert sorted(breaks)==json.loads(r['all_merged_visible_depth_breakpoints_micro'])
            den=U**3;upper=0;fee_upper=0;gross_upper=0
            for q in breaks:
                lower_cost=0;fee_cost=0;gross_cost=0
                for a,l in zip(asks,legs):
                    remaining=q;rate=int(l['fee_rate_micro']);lower_cost+=q*(forward+(conversion if p['venue_scope']=='CROSS_VENUE' else 0))*U**2
                    for price,size in a:
                        take=min(remaining,size);remaining-=take;lower_cost+=take*price*U**2+take*rate*price*(U-price);fee_cost+=take*price*U**2+take*rate*price*(U-price);gross_cost+=take*price*U**2
                    assert remaining==0
                upper=max(upper,q*den-lower_cost);fee_upper=max(fee_upper,q*den-fee_cost);gross_upper=max(gross_upper,q*den-gross_cost)
            assert (fee_upper+den-1)//den==micro(r['coupled_continuous_fee_upper_bound']) and (gross_upper+den-1)//den==micro(r['coupled_continuous_gross_upper_bound']);bound=(upper+den-1)//den;assert bound==micro(r['coupled_continuous_upper_bound']);assert bound>=max(0,n);assert bound-max(0,n)==micro(r['bounded_global_optimality_gap']);bounds+=1
        elif r['exhaustive_valid_discrete_domain']=='true':assert micro(r['bounded_global_optimality_gap'])==0
        else:assert not r['bounded_global_optimality_gap']
        assert not r['strict_pnl'];checked+=1
    return {'quantity_states_checked':checked,'independent_feasible_quantity_vectors':valid_vectors,'coupled_continuous_bounds_verified':bounds,'all_merged_retained_depth_breakpoints_verified':True,'live_orders_sent':0}

def raw_packets(capture):
    manifest=json.loads((capture/'capture_manifest.json').read_text());raw=[]
    for e in manifest['raw_inventory']:
        p=capture/e['file'];assert sha(p)==e['sha256']
        for line in p.open():raw.append(json.loads(line))
    raw.sort(key=lambda r:r['record_sequence']);return manifest,raw

def selection_evidence(capture,analysis,intended):
    manifest,raw=raw_packets(capture);selected=json.loads((capture/'selected_markets.json').read_text());markets={(m['venue'],m['market_id']):m for m in selected};expected={(v,m) for v,ids in intended.items() for m in ids};tokens={}
    for key,m in markets.items():
        for side in ['yes','no']:tokens[m[side]['instrument_id']]=(key,side)
    snapshots=Counter();deltas=Counter();acks=Counter();events=Counter();epochs=defaultdict(set);stream_first={};stream_last={};kseq={};gaps=0;data_times=defaultdict(list)
    for r in raw:
        v=r['venue']
        if r['transport']=='event':events[(v,r['classification'])]+=1
        if r['transport']!='ws':continue
        try:packet=json.loads(r['raw_payload'])
        except ValueError:continue
        if v=='kalshi':
            if not isinstance(packet,dict):continue
            if packet.get('type')=='subscribed':acks[(v,packet['msg']['channel'])]+=1
            if packet.get('type') not in ['orderbook_snapshot','orderbook_delta']:continue
            sid=(r['connection_epoch'],packet['sid']);seq=packet['seq']
            if sid in kseq and seq!=kseq[sid]+1:gaps+=1
            kseq[sid]=seq;m=packet['msg']['market_ticker'];key=(v,m);epochs[key].add(r['connection_epoch']);data_times[v].append(r['recv_monotonic_ns'])
            if packet['type']=='orderbook_snapshot':
                for side in ['yes','no']:snapshots[(key,side)]+=1;stream_first[(key,side,r['connection_epoch'])]=r['recv_monotonic_ns']
            else:
                # A Kalshi bid-side update changes the complementary acquisition ask.
                side='no' if packet['msg']['side']=='yes' else 'yes';deltas[(key,side)]+=1
            for side in ['yes','no']:stream_last[(key,side)]=r['recv_monotonic_ns']
        else:
            for x in packet if isinstance(packet,list) else [packet]:
                if not isinstance(x,dict):continue
                if x.get('event_type')=='book' and x.get('asset_id') in tokens:
                    key,side=tokens[x['asset_id']];snapshots[(key,side)]+=1;epochs[key].add(r['connection_epoch']);stream_first.setdefault((key,side,r['connection_epoch']),r['recv_monotonic_ns']);stream_last[(key,side)]=r['recv_monotonic_ns'];data_times[v].append(r['recv_monotonic_ns'])
                elif x.get('event_type')=='price_change':
                    for c in x.get('price_changes',[]):
                        if c.get('asset_id') in tokens:
                            key,side=tokens[c['asset_id']];deltas[(key,side)]+=1;stream_last[(key,side)]=r['recv_monotonic_ns'];data_times[v].append(r['recv_monotonic_ns'])
    native={}
    for r in table(analysis/'native_instrument_coverage.csv'):
        k=json.loads(r['instrument']);native[(k['venue'],k['market_id'],k['instrument_id'])]=r
    rows=[]
    for key in sorted(set(markets)|expected):
        m=markets.get(key)
        for side in ['yes','no']:
            instrument=m[side] if m else None;health=native.get((key[0],key[1],instrument['instrument_id'])) if m else None
            rows.append({'venue':key[0],'market_id':key[1],'instrument_id':instrument['instrument_id'] if m else None,'outcome':side,'selection_cohort':'INTENDED_NEW' if key in expected else 'FALLBACK_CONTROL','selected':m is not None,'subscription_attempted':True if m else None,'attempt_evidence':'NATIVE_SOURCE_SUBSCRIPTION_LOOP_AND_SELECTED_MARKETS; outbound payload not recorded' if m else None,'subscription_accepted':True if snapshots[(key,side)] else None,'acceptance_evidence':'REAL_ORDERBOOK_SNAPSHOT' if snapshots[(key,side)] else 'NO_PER_INSTRUMENT_ACK_OR_SNAPSHOT','channel_ack_count':acks[(key[0],'orderbook_delta')] if key[0]=='kalshi' else None,'snapshots':snapshots[(key,side)],'acquisition_side_deltas':deltas[(key,side)],'connection_epochs':json.dumps(sorted(epochs[key])),'healthy_asof_sec':health['acquisition_eligible_sec'] if health else None,'both_acquisition_sides_sec':health['both_market_acquisition_sides_eligible_sec'] if health else None,'quantity_minimum_micro':health['minimum_micro'] if health else None,'quantity_step_micro':health['step_micro'] if health else None,'quantity_precision_complete':bool(health and int(health['minimum_micro'])>0 and int(health['step_micro'])>0),'fee_evidence_available':health['fee_evidence_valid'] if health else None,'fee_source':health['fee_source'] if health else None,'rules_hash':m['rules_hash'] if m else None,'source_recording':str(capture),'market_raw_sha256':sha(capture/'selected_markets.json')})
    overlap=max(0,min(max(v) for v in data_times.values())-max(min(v) for v in data_times.values()))/1e9 if len(data_times)==2 else 0
    intervals=table(analysis/'native_coverage_intervals.csv');sums=Counter();calendar=defaultdict(list);sweep=defaultdict(lambda:[0,0]);expected_counts=Counter(v for v,m in expected)
    for interval in intervals:
        key=json.loads(interval['instrument']);a,b=int(interval['start_ns']),int(interval['end_ns']);assert manifest['start_monotonic_ns']<=a<=b<=manifest['end_monotonic_ns'];identity=(key['venue'],key['market_id'],key['instrument_id'],interval['kind']);calendar[identity].append((a,b));sums[identity]+=b-a
        if interval['kind']=='BOTH_ACQUISITION_SIDES' and (key['venue'],key['market_id']) in expected:
            v=0 if key['venue']=='kalshi' else 1;sweep[a][v]+=1;sweep[b][v]-=1
    for periods in calendar.values():
        periods.sort();assert all(a[1]<=b[0] for a,b in zip(periods,periods[1:]))
    for row in rows:
        if row['instrument_id']:
            duration=sums[(row['venue'],row['market_id'],row['instrument_id'],'SINGLE_ACQUISITION_SIDE')]/1e9;assert abs(duration-float(row['healthy_asof_sec']))<1e-6
    active=[0,0];last=manifest['start_monotonic_ns'];joint=all_joint=0
    for t,d in sorted(sweep.items()):
        if min(active)>0:joint+=t-last
        if active==[expected_counts['kalshi'],expected_counts['polymarket_international']]:all_joint+=t-last
        active=[a+b for a,b in zip(active,d)];last=t
    planned=json.loads((Path(__file__).resolve().parents[2]/'runs/cross_venue/v0_1_3_20261106/preflight_final/capture_plan.json').read_text())['selected_relationships'];planned_ids={r['relationship_id'] for r in planned};proofs_path=Path(__file__).resolve().parents[2]/'runs/cross_venue/v0_1_3_20261106/proofs/payoff_proofs.jsonl';proofs,_=read_proofs(proofs_path);relationship_health={r['relationship_id']:r for r in table(analysis/'asof_coverage.csv')}
    for row in rows:
        matches=[rid for rid in planned_ids if any(l['instrument']['venue']==row['venue'] and l['instrument']['market_id']==row['market_id'] and l['instrument']['instrument_id']==row['instrument_id'] for l in proofs[rid]['legs'])];durations=[float(relationship_health[rid].get('eligible_sec') or 0) for rid in matches];row['planned_relationships_requiring_this_outcome']=json.dumps(matches);row['best_planned_counterpart_sync_sec']=max(durations) if durations else None;row['worst_planned_counterpart_sync_sec']=min(durations) if durations else None;row['both_acquisition_sides_available']=float(row['both_acquisition_sides_sec'] or 0)>0
    status={'requested_duration_sec':manifest['requested_duration_sec'],'actual_duration_sec':manifest['actual_duration_sec'],'capture_complete':manifest['requested_duration_sec']==1200 and manifest['actual_duration_sec']>=1199 and not manifest['interrupted'] and manifest['fatal_errors']==0,'kalshi_authenticated_ws_market_data':bool(data_times['kalshi'] and sum(v for (key,side),v in snapshots.items() if key[0]=='kalshi') and sum(v for (key,side),v in deltas.items() if key[0]=='kalshi')),'kalshi_sid_sequence_gaps':gaps,'both_venue_ws_message_span_intersection_sec':overlap,'span_is_not_healthy_asof_coverage':True,'healthy_intended_two_venue_both_side_overlap_sec':joint/1e9,'all_intended_markets_both_sides_healthy_simultaneously_sec':all_joint/1e9,'planned_relationship_groups':len(planned_ids),'planned_groups_with_priced_states':sum(int(relationship_health[rid].get('priced_states') or 0)>0 for rid in planned_ids),'intended_market_count':len(expected),'selected_intended_count':len(expected&set(markets)),'fallback_market_count':len(set(markets)-expected),'events':{'|'.join(k):v for k,v in events.items()},'source_capture_manifest_sha256':sha(capture/'capture_manifest.json'),'live_orders_sent':manifest['live_orders_sent'],'verified_live_realized_pnl':None}
    assert status['live_orders_sent']==0 and gaps==0
    return rows,status

def audit_allocations(rows):
    used=set();wallets=[0,0]
    for r in rows:
        if not r.get('allocated'):continue
        keys=set(r['liquidity_instruments']);assert not keys&used;used|=keys
        wallets[0]+=micro(r['wallet_kalshi']);wallets[1]+=micro(r['wallet_polymarket']);assert r['approval_status']=='PENDING_HUMAN_REVIEW' and not r.get('actual_trade')
    assert max(wallets)<=100000*U and sum(wallets)<=200000*U
    return {'distinct_allocated_instruments':len(used),'wallets_micro':wallets,'duplicate_consumption':False}

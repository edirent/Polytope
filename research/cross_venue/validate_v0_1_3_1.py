"""Bounded V0.1.3.1 evidence orchestration and report generation, no orders."""
import argparse,csv,hashlib,itertools,json,re,shutil,subprocess,time
from collections import Counter,defaultdict
from datetime import datetime,timezone
from decimal import Decimal
from pathlib import Path
from audit_v0_1_3 import audit as base_audit,table,cost_and_fee,micro,U
from audit_v0_1_3_1 import quantity_audit,selection_evidence,audit_allocations
ROOT=Path(__file__).resolve().parents[2]
OLD=ROOT/'runs/cross_venue/v0_1_3_20261106'
PROOFS=OLD/'proofs/payoff_proofs.jsonl'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):return json.loads(p.read_text())
def write(p,v):p.write_text(json.dumps(v,indent=2,ensure_ascii=False)+'\n')
def csvout(p,rows):
    if not rows:rows=[{'status':'UNAVAILABLE','reason':'NO_QUALIFYING_OBSERVATIONS','value':None}]
    keys=sorted(set().union(*(set(r) for r in rows)))
    with p.open('w',newline='') as f:w=csv.DictWriter(f,keys);w.writeheader();w.writerows(rows)
def mdtable(h,rows):return '\n'.join(['| '+' | '.join(h)+' |','| '+' | '.join(['---']*len(h))+' |']+['| '+' | '.join(str(x).replace('|','/') for x in r)+' |' for r in rows])
def native(config,capture,out):
    subprocess.run([str(ROOT/'build/run_generalized_payoff_research'),'--phase','replay','--config',str(config),'--relationships',str(PROOFS),'--run-dir',str(capture),'--out',str(out)],check=True,cwd=ROOT,stdout=subprocess.DEVNULL)
def source_name(p):
    try:return str(p.relative_to(ROOT))
    except ValueError:return str(p)
def prepare(root,capture):
    manifest=read(capture/'capture_manifest.json');assert manifest['requested_duration_sec']==1200 and manifest['actual_duration_sec']>=1199 and not manifest['interrupted'] and not manifest['fatal_errors'];assert manifest['live_orders_sent']==0
    cfg=read(ROOT/'config/cross_venue_v0_1_3_1.json');config=root/'baseline_config.json';write(config,cfg);analysis=root/'baseline'
    if not analysis.exists():
        native(config,capture,analysis);audit=base_audit(PROOFS,capture,analysis);write(root/'baseline_audit.json',audit)
    else:
        assert read(analysis/'input_capture_manifest.json')==manifest and read(analysis/'analysis_config.json')==cfg;assert read(root/'baseline_audit.json')['status']=='INDEPENDENT_GENERALIZED_ECONOMIC_AUDIT_PASSED'
    proofs={p['relationship_id']:p for p in map(json.loads,PROOFS.read_text().splitlines())};intended=read(capture/'capture_config.json')['capture_market_ids'];expected={(v,m) for v,ms in intended.items() for m in ms};selected={(m['venue'],m['market_id']) for m in read(capture/'selected_markets.json')}
    def cohort(rid):
        mids={(l['market']['venue'],l['market']['market_id']) for l in proofs[rid]['legs']}
        return 'INTENDED_NEW' if mids<=expected else ('MIXED_INTENDED_CONTROL' if mids&expected else ('FALLBACK_CONTROL' if mids<=selected else 'NOT_IN_CAPTURE'))
    prior=[('historical_v011',OLD/'analysis/historical_v011'),('historical_v012',OLD/'analysis/historical_v012')]
    captures=[('new_authenticated',analysis)]+prior;quoted=[];byid={};sourcecosts={}
    for name,p in captures:
        for r in table(p/'portfolio_candidates.csv'):
            r=dict(r,capture_id=name,selection_cohort=cohort(r['relationship_id']) if name=='new_authenticated' else 'HISTORICAL_CONTROL');quoted.append(r);byid[(name,r['portfolio_id'])]=r
        costs=defaultdict(list)
        for r in table(p/'portfolio_costs.csv'):costs[r['portfolio_id']].append(r)
        sourcecosts[name]=costs
    rank=[]
    for rid,p in proofs.items():
        rs=[r for r in quoted if r['relationship_id']==rid];new=[r for r in rs if r['capture_id']=='new_authenticated'];observed=new or rs
        if not observed:continue
        best=max(observed,key=lambda r:micro(r['max_feasible_fee_margin']));gross=max(micro(r['max_feasible_gross_margin']) for r in observed);fee=max(micro(r['max_feasible_fee_margin']) for r in observed);budget=max(micro(r['budget_adjusted_margin']) for r in observed)
        horizon=min((datetime.fromisoformat(l['market']['close_time'].replace('Z','+00:00')).timestamp() for l in p['legs']),default=0)
        rank.append({'relationship_id':rid,'type':p['type'],'selection_cohort':cohort(rid) if new else 'HISTORICAL_CONTROL','capture_id':best['capture_id'],'priced_states':len(observed),'gross_best':str(Decimal(gross)/U),'fee_best':str(Decimal(fee)/U),'budget_best':str(Decimal(budget)/U),'normal_rule_supported':p['justified_normal_minimum']>0,'leg_count':len(p['legs']),'visible_depth_micro':best['visible_depth_micro'],'trading_close_proxy':min(l['market']['close_time'] for l in p['legs']),'capital_release':None,'complete_proof':False,'approval_status':'PENDING_HUMAN_REVIEW','reference_portfolio_id':best['portfolio_id'],'_score':(fee>0,gross>0,bool(new),fee,min(json.loads(best['visible_depth_micro'])),-horizon,p['justified_normal_minimum']>0,-len(p['legs']))})
    rank.sort(key=lambda r:(r['_score'],r['relationship_id']),reverse=True)
    # Choose up to five economical, non-duplicative structural research dossiers.
    priorities=[];seen=set();selected_types=Counter()
    for r in rank:
        p=proofs[r['relationship_id']];signature=(r['selection_cohort'],p['type'],p['legs'][0]['market']['event_id'])
        if signature in seen or selected_types[p['type']]>=2:continue
        seen.add(signature);priorities.append(r);selected_types[p['type']]+=1
        if len(priorities)==5:break
    for i,r in enumerate(rank):r['economic_rank']=i+1;r['intensive_review_selected']=r in priorities;r.pop('_score')
    csvout(root/'proof_review_priority.csv',rank)
    # Verify a deterministic, economically targeted subset; no blanket solver expansion.
    chosen=[]
    for pr in priorities:
        if pr['capture_id']!='new_authenticated':continue
        rs=[r for r in quoted if r['capture_id']=='new_authenticated' and r['relationship_id']==pr['relationship_id']];rs.sort(key=lambda r:(micro(r['max_feasible_fee_margin']),micro(r['max_feasible_gross_margin']),-int(r['decision_ns'])),reverse=True)
        fingerprints=set()
        for r in rs:
            legs=sourcecosts['new_authenticated'][r['portfolio_id']];fp=tuple((l['instrument'],l['visible_asks'],l['fee_version']) for l in sorted(legs,key=lambda l:int(l['leg_index'])))
            if fp in fingerprints:continue
            fingerprints.add(fp);chosen.append(r['portfolio_id'])
            if len(fingerprints)>=12:break
    # Include promising observed 3/4-leg templates to measure incremental active-leg value.
    for legs in [3,4]:
        rs=[r for r in quoted if r['capture_id']=='new_authenticated' and int(r['configured_legs'])==legs];rs.sort(key=lambda r:(micro(r['max_feasible_fee_margin']),-int(r['decision_ns'])),reverse=True)
        used=set()
        for r in rs:
            if r['relationship_id'] in used:continue
            chosen.append(r['portfolio_id']);used.add(r['relationship_id'])
            if len(used)>=2:break
    if chosen:
        cfg['quantity_verification_portfolio_ids']=sorted(set(chosen));cfg['quantity_verification_expand_to_visible_depth']=True;write(root/'verification_config.json',cfg);native(root/'verification_config.json',capture,root/'verification');shared=[]
        for f in analysis.iterdir():
            if f.name=='analysis_config.json':continue
            assert f.read_bytes()==(root/'verification'/f.name).read_bytes(),f.name;shared.append({'file':f.name,'sha256':sha(f)})
        write(root/'verification_base_audit.json',{'status':'REUSED_VERIFIED_IDENTICAL_BASELINE_CORE','baseline_audit_sha256':sha(root/'baseline_audit.json'),'files':shared});write(root/'quantity_audit.json',quantity_audit(PROOFS,root/'verification'))
        native(root/'verification_config.json',capture,root/'verification_repeat');compared=0
        for p in (root/'verification').iterdir():
            if p.name=='quantity_solver_timings.jsonl':continue
            assert p.read_bytes()==(root/'verification_repeat'/p.name).read_bytes(),p.name;compared+=1
        write(root/'determinism_audit.json',{'status':'PASS','core_files_byte_compared':compared,'excluded_measured_performance_file':'quantity_solver_timings.jsonl','reason':'Wall-clock performance is measured separately and is not a deterministic economic input'})
    else:write(root/'quantity_audit.json',{'status':'NOT_EXECUTED_NO_TARGET_WITH_REAL_BOOK_EVIDENCE'})
    finalize(root,capture,proofs,quoted,sourcecosts,priorities,cohort,rank)

def finalize(root,capture,proofs,quoted,sourcecosts,priorities,cohort,rank):
    analysis=root/'baseline';selection,status=selection_evidence(capture,analysis,read(capture/'capture_config.json')['capture_market_ids']);csvout(root/'selected_market_coverage.csv',selection);csvout(root/'subscription_validation.csv',selection)
    manifests=read(capture/'capture_manifest.json');write(root/'capture_manifest.json',{'schema':'v0.1.3.1-reused-immutable-native-capture','source_recording':str(capture),'source_manifest':str(capture/'capture_manifest.json'),'source_manifest_sha256':sha(capture/'capture_manifest.json'),'raw_inventory_referenced_not_copied':manifests['raw_inventory'],**status})
    fresh=table(analysis/'portfolio_candidates.csv');pmap={r['portfolio_id']:r for r in fresh}
    def enrich(r,source='new_authenticated'):
        r=dict(r);rid=r.get('relationship_id');p=proofs.get(rid);r.update(capture_id=source,selection_cohort=cohort(rid) if rid and source=='new_authenticated' else 'HISTORICAL_CONTROL',source_recording=str(capture) if source=='new_authenticated' else read(OLD/'analysis'/source/'economic_summary.json')['source_capture'])
        if p:r.update(instruments=json.dumps([l['instrument'] for l in p['legs']]),rules_revision=p['revision'],artifact_sha256=p['artifact_sha256'],proof_status=p['proof_status'],approval_status=p['approval_status'])
        r['source_costs_file']=str((analysis if source=='new_authenticated' else OLD/'analysis'/source)/'portfolio_costs.csv');return r
    for src,dst in [('asof_coverage','asof_coverage'),('asof_coverage','relationship_coverage'),('opportunity_episodes','opportunity_episodes'),('execution_risk','execution_risk'),('latency_sensitivity','latency_sensitivity')]:csvout(root/(dst+'.csv'),[enrich(r) for r in table(analysis/(src+'.csv'))])
    csvout(root/'payoff_relationships.csv',[dict(relationship_id=k,type=p['type'],instruments=json.dumps([l['instrument'] for l in p['legs']]),rules_revision=p['revision'],artifact_sha256=p['artifact_sha256'],normal_minimum_micro=p['hypothetical_normal_minimum'],normal_supported_minimum_micro=p['justified_normal_minimum'],missing_states=json.dumps(p['missing_states']),full_contract_proof=False,approval_status=p['approval_status']) for k,p in proofs.items()])
    csvout(root/'portfolio_opportunities.csv',[enrich(r) for r in fresh])
    verification=root/'verification';checks=table(verification/'quantity_breakpoint_analysis.csv') if verification.exists() else [];witnesses=table(verification/'quantity_verification_witnesses.csv') if verification.exists() else []
    csvout(root/'quantity_breakpoint_analysis.csv',[enrich(r) for r in checks]);csvout(root/'quantity_verification_witnesses.csv',[enrich(r) for r in witnesses])
    if verification.exists():shutil.copyfile(verification/'quantity_solver_timings.jsonl',root/'quantity_solver_timings.jsonl')
    candidate_rows=[]
    for r in table(analysis/'fee_adjusted_opportunities.csv'):candidate_rows.append(enrich(r))
    for r in witnesses:
        if micro(r['fee_adjusted'])>0 or micro(r['budget_adjusted'])>0:candidate_rows.append(enrich(dict(r,fee_adjusted_margin=r['fee_adjusted'],budget_adjusted_margin=r['budget_adjusted'],source='ENHANCED_QUANTITY_WITNESS')))
    # Conservative allocation: hold each instrument to the end, no assumed refill.
    # A bounded exact independent-set allocator is used when a positive set exists.
    options={}
    for r in candidate_rows:
        if micro(r['budget_adjusted_margin'])<=0:continue
        p=proofs[r['relationship_id']];key=tuple(sorted(json.dumps(l['instrument'],sort_keys=True) for l in p['legs']));value=micro(r['budget_adjusted_margin'])
        if key not in options or value>micro(options[key]['budget_adjusted_margin']):options[key]=dict(r,liquidity_instruments=list(key),actual_trade=False)
    options=list(options.values());options.sort(key=lambda r:micro(r['budget_adjusted_margin']),reverse=True);truncated=len(options)>20;options=options[:20];best=0;allocation=[]
    def allocate(i,used,spent,profit,picked):
        nonlocal best,allocation
        if profit>best:best=profit;allocation=picked[:]
        if i==len(options):return
        if profit+sum(micro(r['budget_adjusted_margin']) for r in options[i:])<=best:return
        r=options[i];keys=set(r['liquidity_instruments']);money=[micro(r['wallet_kalshi']),micro(r['wallet_polymarket'])]
        if not used&keys and all(a+b<=100000*U for a,b in zip(spent,money)) and sum(spent)+sum(money)<=200000*U:allocate(i+1,used|keys,[a+b for a,b in zip(spent,money)],profit+micro(r['budget_adjusted_margin']),picked+[i])
        allocate(i+1,used,spent,profit,picked)
    allocate(0,set(),[0,0],0,[])
    for i,r in enumerate(options):r['allocated']=i in allocation
    allocation_audit=audit_allocations(options);csvout(root/'deduplicated_opportunity_ledger.csv',options);write(root/'allocation_audit.json',dict(allocation_audit,tested_net_capacity_nominal=str(Decimal(best)/U),allocation_search_truncated=truncated,raw_records_never_consumed_by_diagnostics=True,refill_assumed=False))
    csvout(root/'fee_adjusted_capacity.csv',[enrich(r) for r in candidate_rows])
    capital=[enrich(r) for r in table(analysis/'capital_efficiency.csv')];csvout(root/'capital_requirements.csv',capital)
    annual=[{'annual_target':t,'assumed_net_edge':str(e),'required_notional_turnover':str(Decimal(t)/e),'full_200k_capital_turns':str(Decimal(t)/e/200000),'status':'HYPOTHETICAL_NOT_EMPIRICAL'} for t in [10000,25000,50000,100000] for e in [Decimal('.001'),Decimal('.005'),Decimal('.01')]];csvout(root/'annual_target_arithmetic.csv',annual)
    dossiers(root,proofs,priorities)
    shutil.copyfile(OLD/'proofs/approval_manifest.pending.json',root/'approval_manifest.pending.json')
    classes=[('Cross-Venue Double Taker',lambda r:r['scope']=='CROSS_VENUE' and int(r['configured_legs'])==2),('Same-Venue Combinatorial',lambda r:r['scope']=='SAME_VENUE'),('Logical Implication',lambda r:r['type'] in ['NESTED_THRESHOLD','NESTED_TIME_WINDOW','THREE_LEG_LADDER','FOUR_LEG_LADDER']),('Multi-Leg',lambda r:int(r['configured_legs'])>2)]
    strategies=[]
    for name,predicate in classes:
        for group in ['INTENDED_NEW','FALLBACK_CONTROL','MIXED_INTENDED_CONTROL']:
            rs=[r for r in fresh if predicate(r) and cohort(r['relationship_id'])==group];strategies.append({'strategy':name,'selection_cohort':group,'priced_states':len(rs),'distinct_relationships':len({r['relationship_id'] for r in rs}),'gross_positive':sum(r['any_gross_positive']=='true' for r in rs),'fee_positive':sum(r['any_fee_positive']=='true' for r in rs),'budget_positive':sum(r['any_budget_positive']=='true' for r in rs),'active_three_legs':sum(int(r['active_legs'])==3 for r in rs),'active_four_legs':sum(int(r['active_legs'])==4 for r in rs),'strict_pnl':None})
    maker=table(analysis/'maker_feasibility.csv');strategies.append({'strategy':'Maker-First Feasibility','selection_cohort':'ALL_CAPTURED_SEPARATELY_TAGGED_IN_NATIVE_ROWS','quote_feasible':sum(r.get('maker_quote_feasible')=='true' for r in maker),'quote_competitive':sum(r.get('maker_quote_competitive')=='true' for r in maker),'maker_fill_probability':None,'maker_realized_pnl':None});csvout(root/'strategy_comparison.csv',strategies)
    report(root,capture,status,proofs,fresh,checks,witnesses,strategies,priorities,best,quoted)
    seal(root,capture,proofs,priorities)

def dossiers(root,proofs,priorities):
    sources=read(root/'authoritative_sources/requests.json');available=[s for s in sources if s['http_status']==200];branches=['TIE','VOID','CANCELLATION','POSTPONEMENT','EARLY_TERMINATION','DISPUTE','REPLACEMENT','FRACTIONAL_REFUND','SOURCE_DIVERGENCE','UNDEFINED']
    for i,r in enumerate(priorities,1):
        p=proofs[r['relationship_id']];markets=[l['market'] for l in p['legs']];names=' '.join(m['market_id'] for m in markets);notes=[]
        if 'KXCPI' in names:notes.append('CPI条款定义同一BLS月度数值和严格大于阈值，并有缺数据的备用计算；仍须核实规则修改、争议与自主支付条款的联合约束。交易关闭不是现金释放。')
        if 'KXRT' in names:notes.append('RT条款有延后取值与持续缺数据时所有市场NO分支。阈值梯形需显式验证该分支，不得直接排除；Rule 6.3审查与替代支付仍未封闭。')
        if 'KXNBAGAME' in names:notes.append('篮球条款规定同分、48小时延期、取消、中断、弃权与缺数据；部分分支按交易所确定的最后公平价格。不同市场公平价格的联合约束未取得，不能只套二元互斥矩阵。')
        if 'KXALIENS' in names:notes.append('两侧发布主体范围、可信报道和扩展Cabinet定义需要逐项比对。交易关闭时间差不是直接的支付反例；不能将close等同事件截止时间。原gross信号仍扣费为负且长期锁资。')
        if any(m['venue']=='polymarket_international' for m in markets):notes.append('官方说明UMA可出现未知/50-50结算，且可有链上澄清；pUSD/条件代币结构只能提供资产候选，尚无该condition与实际抵押品合约的完整绑定。NegRisk不视为任意跨市场可转换证明。')
        statuses=[{'branch':b,'status':'KNOWN_SPECIFIC_CASE_NOT_EXHAUSTIVE' if b=='CANCELLATION' and p.get('exceptional_payoffs') else 'UNRESOLVED_COMPLETE_JOINT_BRANCH','blocking_source':'Effective global rulebook / condition-bound contract and collateral evidence / exact joint payout linkage','must_not_assume_impossible':True} for b in branches]
        dossier={'schema':'polytope-intensive-review-package-v0.1.3.1','relationship_id':p['relationship_id'],'original_revision':p['revision'],'original_artifact_sha256':p['artifact_sha256'],'economic_priority':r,'original_native_proof':p,'authoritative_sources':available,'failed_sources':[s for s in sources if s['http_status']!=200],'review_notes':notes,'settlement_branch_review':statuses,'asset_identity_verified':False,'quantity_constraints_complete':False,'capital_release_bound':None,'ordinary_model_status':p['mathematical_status'],'full_contractual_proof_status':'INCOMPLETE_SETTLEMENT_COVERAGE','complete_proof_candidate':False,'human_approved':False,'approval_status':'PENDING_HUMAN_REVIEW','effective_rule_version_binding':'URL and downloaded hash recorded; no unproven retroactive version attestation','counterexamples':{'normal_minimum_witness':p['normal_minimum_witness'],'normal_model_floor_micro':p['hypothetical_normal_minimum'],'unresolved_exception_envelope_floor_micro':p['conservative_lower_envelope_minimum'],'envelope_is_not_observed_exchange_outcome':True}}
        filename=f'{i:02}_{p["relationship_id"]}';write(root/'full_rule_dossiers'/(filename+'.json'),dossier)
        text=f'# Intensive rule review {i}\n\nRelationship: `{p["relationship_id"]}`; original revision {p["revision"]}; artifact `{p["artifact_sha256"]}`.\n\n'+ '\n\n'.join(notes)+'\n\nFull contractual proof: INCOMPLETE. Human approval: PENDING. Detailed original rules, state vectors, missing branches, authoritative URLs/hashes, failed fetches and exact blockers are in the paired JSON. Original proof and pending approval artifact are unchanged.\n'
        (root/'full_rule_dossiers'/(filename+'.md')).write_text(text)
    write(root/'full_rule_dossiers/index.json',{'dossiers':len(priorities),'full_contractual_proofs':0,'human_approvals':0,'source_policy':'Official exchange API metadata, official contract PDFs and official documentation only'})

def report(root,capture,status,proofs,fresh,checks,witnesses,strategies,priorities,capacity,quoted):
    selection=table(root/'selected_market_coverage.csv');coverage=table(root/'relationship_coverage.csv');healthy={r['relationship_id'] for r in coverage if int(r.get('priced_states') or 0)>0};matched={r['relationship_id'] for r in coverage if r.get('covered_instruments')=='true'};historical={r['relationship_id'] for r in quoted}
    # Every intended/new metric below is based on the cohort field in the exported tables.
    intended_rows=[r for r in table(root/'portfolio_opportunities.csv') if r['selection_cohort']=='INTENDED_NEW'];episodes=table(root/'opportunity_episodes.csv');newfee=sum(r['new_fee_positive']=='true' for r in checks);newnet=sum(r['new_budget_positive']=='true' for r in checks);newgross=sum(r['new_gross_positive']=='true' for r in checks);bounded_zero=sum(r.get('bounded_global_optimality_gap')=='0.000000' for r in checks);three_improve=[r for r in checks if r['best_three_active_net'] and r['best_two_active_net'] and micro(r['best_three_active_net'])>micro(r['best_two_active_net'])];four_improve=[r for r in checks if r['best_four_active_net'] and r['best_two_active_net'] and micro(r['best_four_active_net'])>micro(r['best_two_active_net'])]
    active=Counter(int(r['active_legs']) for r in fresh);tests=read(root/'test_results.json') if (root/'test_results.json').exists() else {};reco='INSUFFICIENT_EVIDENCE'
    results={'schema':'polytope-real-market-validation-v0.1.3.1','recommendation':reco,'capture':status,'candidates':len(proofs),'matched_new_capture_relationships':len(matched),'healthy_new_capture_relationships':len(healthy),'usable_relationships_union_with_historical':len(historical),'new_capture_priced_states':len(fresh),'intended_new_priced_states':len(intended_rows),'intended_gross_positive':sum(r['any_gross_positive']=='true' for r in intended_rows),'intended_fee_positive':sum(r['any_fee_positive']=='true' for r in intended_rows),'intended_budget_positive':sum(r['any_budget_positive']=='true' for r in intended_rows),'all_capture_gross_positive':sum(r['any_gross_positive']=='true' for r in fresh),'all_capture_fee_positive':sum(r['any_fee_positive']=='true' for r in fresh),'all_capture_budget_positive':sum(r['any_budget_positive']=='true' for r in fresh),'enhanced_search_states':len(checks),'new_gross_positive_vs_sparse':newgross,'new_fee_positive_vs_sparse':newfee,'new_budget_positive_vs_sparse':newnet,'bounded_quantity_decisions_with_zero_proven_gap':bounded_zero,'three_active_improves_best_nonzero_two_active_states':len(three_improve),'four_active_improves_best_nonzero_two_active_states':len(four_improve),'complete_contractual_proofs':0,'human_approvals':0,'review_dossiers':len(priorities),'active_leg_counts':active,'conditional_nonduplicated_tested_net_capacity':str(Decimal(capacity)/U),'strict_approved_executable_capacity':None,'strict_simulated_pnl':None,'verified_live_realized_pnl':None,'live_orders_sent':0,'tests':tests};write(root/'economic_summary.json',results)
    lags=[]
    for lag in [0,100,250,500,1000]:
        rs=[r for r in table(root/'latency_sensitivity.csv') if r['comparison_basis']=='NET_OPTIMIZER' and int(r['latency_ms'])==lag];lags.append([lag,len(rs),sum(r['status']=='UNKNOWN_UNAVAILABLE' for r in rs),sum(r['net_positive_survives']=='true' for r in rs)])
    text=f'''# Polytope V0.1.3.1 — 实际市场验证与经济判断

主建议：**{reco}**。本轮复用已安全启动的同一采集，未重复启动实验；原始数据引用不复制。真实订单0，`verified_live_realized_pnl = null`。未批准组合仅作条件诊断。

## 1. 新市场是否真的获得认证L2？

采集实际 {status['actual_duration_sec']:.6f} 秒，Kalshi认证WS市场数据证据 `{status['kalshi_authenticated_ws_market_data']}`，完整性 `{status['capture_complete']}`。{status['intended_market_count']} 个预定市场中实际选中 {status['selected_intended_count']} 个，另有 {status['fallback_market_count']} 个控制市场。逐token的快照、买入侧增量、As-Of双侧覆盖、数量约束、费用和规则哈希见 `selected_market_coverage.csv`。未因控制市场有流量就宣称目标被覆盖。

两场所WS首末市场消息时间跨度的交集 {status['both_venue_ws_message_span_intersection_sec']:.6f} 秒，这只是消息跨度，不冒充健康As-Of覆盖。真正可买单腿/双侧覆盖来自原生State/AsOf按每个失效界切分的区间：至少一个预定Kalshi市场与一个预定Polymarket市场同时双侧健康的时间为 {status['healthy_intended_two_venue_both_side_overlap_sec']:.6f} 秒；全部预定市场同时双侧健康为 {status['all_intended_markets_both_sides_healthy_simultaneously_sec']:.6f} 秒。11个预定结构组中有 {status['planned_groups_with_priced_states']} 个获得报价状态。关系所需对手腿同步覆盖见 `relationship_coverage.csv`。订阅尝试由选中名单与原生订阅循环支持；原始出站订阅payload未保存，逐token接受由真实snapshot证明，Polymarket无显式ACK时不伪造ACK。Kalshi sid/seq gaps={status['kalshi_sid_sequence_gaps']}。断开/恢复事件：`{status['events']}`，600秒的计划重连单独标识。

## 2. 191个关系有多少真实盘口证据？

新录制中 {len(matched)} 个候选的全部原合约/规则版本可匹配，{len(healthy)} 个有健康可报价状态，共 {len(fresh):,} 状态；与历史合计 {len(historical)} 个候选有可报价证据。缺数据/规则版本不符保留未知，不能记为无机会。预定新样本单列 {len(intended_rows):,} 状态，未将补位控制组并入该数字。`payoff_relationships.csv`保留全部191个关系及原证明revision/hash。

## 3–5. 毛利、扣费与预算

{mdtable(['范围','可报价状态','毛利正','扣费正','预算正'],[['预定新结构',len(intended_rows),results['intended_gross_positive'],results['intended_fee_positive'],results['intended_budget_positive']],['本轮全部，含明确控制',len(fresh),results['all_capture_gross_positive'],results['all_capture_fee_positive'],results['all_capture_budget_positive']]])}

五档延迟固定原数量，未来缺盘口记未知，不重挑数量、不当填单：

{mdtable(['延迟ms','状态','未知后续','预算正存活'],lags)}

本轮基线在费用前已经没有正价差，故不能将缺少净机会单归因于手续费。实际市场系数、深度和费用版本来自原始录制。V0.1.3费用函数保留为保守现金预留；官方Kalshi说明同一订单的填单会积累舍入返还，因此分档逐次cent-ceiling不等于已观察的实际最终费用。账户精度与订单填单明细缺失时不虚构准确收据；连续模型费下界另用于可靠的数量范围上界。Polymarket按冻结市场fee参数，不按类别硬编码；[Kalshi舍入文档](https://docs.kalshi.com/getting_started/fee_rounding)、[Polymarket费用](https://docs.polymarket.com/trading/fees)。

## 6. 稀疏数量网格漏了什么？

针对经济优先档案及有数据的多腿模板，确定性选择 {len(checks)} 个不同实盘口状态，双腿复核不再受基线每腿1,000份稀疏量级限制，而扩展到所保留可见深度，并保留钱包/风险限额；复核合并后的深度累计拐点、最小量附近256个步长、舍入临界邻域和钱包边界。基线之外新增毛利正 {newgross}、扣费正 {newfee}、预算正 {newnet} 个状态。{bounded_zero} 个状态的限定数量框内零/非零最优差距有零证书；其余差距未知。只核验被选状态，不宣称搜索了所有时刻、191个关系和任意深度。

二腿支付模型经穷举确认为min(q1,q2)时，利用非递减买入现金建立等量连续成本/模型费下界，合并两腿所有可见深度拐点；段内目标是仿射函数，端点最大值是整个限定数量框的上界。它忽略最小量/钱包/增量约束，故不会把理论上界当可成交机会。只有上界=放弃目标0时才可证明该框内无正的条件预算目标。小多腿仅在已知真实离散域足够小时全枚举，否则保留网格及未知gap。缺少step/min仍是条件诊断，绝不成为可交易量。

所有精确测试向量、上界、最佳非零/含放弃解、两/三/四非零腿值在 `quantity_breakpoint_analysis.csv`，可核对到原始cost表；实测耗时单独在 `quantity_solver_timings.jsonl`，不伪装成确定性字段。合成回归能找回刻意让稀疏网格漏掉的0.30正值，但合成结果从不计入本节真实经济数字。

## 7. 第三、第四腿是否增加价值？

本轮21个已覆盖关系全部为同场所结构；新短周期跨场所双taker关系数为0，不能用本轮零信号否定未覆盖的跨场所机会。本轮基线非零腿计数 `{dict(active)}`。增强复核中有 {len(three_improve)} 个状态的真正三腿组合优于同状态最佳非零双腿，四腿改善状态 {len(four_improve)} 个；“改善”仍可能只是少亏，所有含零仓位的最优决策仍须单列，不能把负值改善称作盈利。必须同时报告配置腿数与非零腿数；增强搜索的 `best_two_active_net`、`best_three_active_net`、`best_four_active_net`直接比较同一状态的条件收益。缺失全离散域时不声称无多腿机会；普通四腿模板若最优仅两腿，不算四腿经济成果。实际成交、maker队列、填单概率均未知，maker realized PnL=null。

## 8. 哪些关系足够人工批准？

密集复核档案 {len(priorities)} 个，选择按已观察扣费/毛利、真实深度、新样本、普通规则支持和独立腿数排序，同时剔除完全重复结构。原始关闭时间只作期限代理，实际资金释放未知，不假称短锁资。当前完整合约证明0、足以无保留提出批准的关系0、人工批准0。

{mdtable(['关系','类型','样本','最好毛利','最好扣费','最好预算'],[[p['relationship_id'][:12],p['type'],p['selection_cohort'],p['gross_best'],p['fee_best'],p['budget_best']] for p in priorities])}

`full_rule_dossiers/`保存原始完整规则、状态向量、官方完整条款PDF/文档哈希、每个特殊分支和精确缺源。规则包保留原revision绑定，不偷偷修改原冻结证明；`approval_manifest.pending.json`未伪造审核人或时间。官方条款不是简单二元假设：[篮球完整条款](https://assets.kalshi.com/contract_terms/BASKETBALLGAMEWIN.pdf)、[CPI条款](https://assets.kalshi.com/contract_terms/CPI.pdf)、[电影评分条款](https://assets.kalshi.com/contract_terms/RT.pdf)。

当前Kalshi监管页面HTTP429，尝试的v1.29官方存档路径404；最新全局规则、版本效力和自主公平价支付的联合约束未取得，不能以过时或猜测文件补齐。Polymarket官方明确UMA可能50/50结算及后续澄清；资产文档提供pUSD结构，但未验证每个condition对应实际抵押品/NegRisk转换的完整链上绑定：[结算规则](https://docs.polymarket.com/concepts/resolution)、[资产定义](https://docs.polymarket.com/concepts/pusd)。

## 9. 非重复净容量与资金风险

已测试并经保守不重复分配的条件净容量 `{Decimal(capacity)/U}` 名义美元等值。这个值只覆盖已报价/已测试数量，严格已批准可部署容量仍为null。不能把多个策略别名或重复报价当独立机会。

`deduplicated_opportunity_ledger.csv`与`allocation_audit.json`通过instrument集合识别共享流动性。若有正预算候选，最多20个互斥组做有界精确子集分配，超过限制明确截断；每个instrument持有到窗口结束，不假设补单或现金周转。候选相同底层事件、抵押品和规则依赖均保留，数量不代表独立风险。负的最佳非零组合对应选择零仓位，不是亏损交易。

资本上限200k、独立钱包默认各100k，沿用原配置；不把可用本金等同深度容量。中间腿现金风险和60%部分填单情景见 `execution_risk.csv`；资金释放、实际填单和真实资本日收益未知。没有将20分钟年化。年度10k/25k/50k/100k目标的0.1%/0.5%/1%净边际所需周转单独在 `annual_target_arithmetic.csv`，是算术要求。

## 10. 下一周期与置信度

{mdtable(['策略','样本','可报价状态','毛利正','扣费正','预算正'],[[s['strategy'],s.get('selection_cohort'),s.get('priced_states','—'),s.get('gross_positive','—'),s.get('fee_positive','—'),s.get('budget_positive','—')] for s in strategies])}

唯一主建议：**{reco}**。已完成新样本认证取证，但一次20分钟窗口、有限候选和不完整合约分支仍不足以证明可重复净利润或200k部署能力。算术/来源/盘口重建在审计范围内置信度高；合约全分支、资产绑定、真实填单、资金释放与规模化证据不足。不要将未覆盖关系、未知数量域和未批准组合视为死策略。

## 测试与独立审计

{json.dumps(tests,ensure_ascii=False)}

已复用V0.1.3原始消息/费用/时序/支付独立审计，附加预定身份/真实认证数据、所有合并深度拐点、数量向量、连续上界、固定数量延迟和共享流动性核对。基线/增强搜索源码、配置、证据哈希、实测性能与确定性范围见 `audit_manifest.json`；原始大文件只引用，不复制。
'''
    (root/'economic_feasibility.md').write_text(text)
    manifest={'schema':'polytope-v0.1.3.1-final-audit','recommendation':reco,'source_capture_manifest_sha256':sha(capture/'capture_manifest.json'),'native_research_binary_sha256':sha(ROOT/'build/run_generalized_payoff_research'),'frozen_proofs_sha256':sha(PROOFS),'selection_and_authentication':status,'baseline_independent_audit':read(root/'baseline_audit.json'),'quantity_independent_audit':read(root/'quantity_audit.json'),'determinism':read(root/'determinism_audit.json') if (root/'determinism_audit.json').exists() else None,'raw_evidence_reference':str(capture),'source_capture_inventory':read(capture/'capture_manifest.json')['raw_inventory'],'source_code':[],'outputs':[],'tests':tests,'verified_live_realized_pnl':None,'live_orders_sent':0}
    changed=['engine/strategy/cross_venue/GeneralizedQuantityVerification.cpp','engine/strategy/cross_venue/GeneralizedResearch.h','engine/strategy/cross_venue/GeneralizedReplay.cpp','engine/strategy/cross_venue/CMakeLists.txt','tests/integration/cross_venue/generalized_payoff_test.cpp','tests/integration/cross_venue/v0131_quantity_audit_test.py','research/cross_venue/audit_v0_1_3_1.py','research/cross_venue/validate_v0_1_3_1.py','config/cross_venue_v0_1_3_1.json','docs/cross_venue_v0_1_3_1.md']
    for name in changed:manifest['source_code'].append({'file':name,'sha256':sha(ROOT/name)})
    for p in sorted(root.rglob('*')):
        if p.is_file() and p.name!='audit_manifest.json' and not p.name.endswith('.pyc'):manifest['outputs'].append({'file':str(p.relative_to(root)),'sha256':sha(p),'bytes':p.stat().st_size})
    write(root/'audit_manifest.json',manifest);print(json.dumps(results,ensure_ascii=False,indent=2))
def seal(root,capture,proofs,priorities):
    """Finalize human-review provenance and immutable evidence hashes."""
    selected={r['relationship_id'] for r in priorities};directory=root/'full_rule_dossiers';entries=[]
    for p in list(directory.glob('*')):
        if p.stem=='index':continue
        if not any(rid in p.name for rid in selected):
            archive=root/'intermediate_sparse_limit/superseded_review_packages';archive.mkdir(parents=True,exist_ok=True);p.rename(archive/p.name)
    for p in sorted(directory.glob('*.json')):
        if p.stem=='index':continue
        d=read(p);proof=proofs[d['relationship_id']];markets=[l['market'] for l in proof['legs']];names=' '.join(m['market_id'] for m in markets)
        d['asset_identity_assessment']={'same_kalshi_nominal_usd_contract_cash':all(m['venue']=='kalshi' for m in markets),'original_native_artifact_not_modified':True,'cross_asset_or_condition_collateral_binding_verified':False}
        d['quantity_constraints_by_instrument']=[{'instrument':l['instrument'],'minimum_quantity':l['market']['minimum_quantity'] if micro(l['market']['minimum_quantity']) else None,'quantity_step':l['market']['quantity_step'] if micro(l['market']['quantity_step']) else None,'missing_minimum_or_step_blocks_strict_orders':not (micro(l['market']['minimum_quantity']) and micro(l['market']['quantity_step']))} for l in proof['legs']]
        for branch in d['settlement_branch_review']:
            b=branch['branch'];branch['native_uncertainty_envelope_retained']=True
            family='BASKETBALLGAMEWIN' if 'KXNBAGAME' in names else ('RT' if 'KXRT' in names else ('CPI' if 'KXCPI' in names else ('ALIENS' if 'KXALIENS' in names else None)))
            branch['authoritative_contract_source_refs']=[{'name':source['name'],'url':source['url'],'sha256':source['sha256'],'file':source['file']} for source in d['authoritative_sources'] if source['name']==family or (any(m['venue']=='polymarket_international' for m in markets) and source['name'] in ['poly_resolution','poly_assets','poly_positions'])]
            if 'KXNBAGAME' in names:
                if b=='TIE':branch['source_finding']='Basketball terms define fractional tie payouts depending on whether a Tie strike is listed; joint branch binding still requires review.'
                if b in ['CANCELLATION','POSTPONEMENT','EARLY_TERMINATION','FRACTIONAL_REFUND']:branch['source_finding']='Basketball terms contain 48-hour and fair-price cases. Joint fair-price coupling across separate contracts is not established.'
                if b=='REPLACEMENT':branch['source_finding']='Team identity follows officially recognized franchise changes; all replacement contingencies not proven closed.'
            if 'KXRT' in names and b=='POSTPONEMENT':branch['source_finding']='RT terms provide deferred observations and an all-NO case after persistent missing data; other global contingencies unresolved.'
            if 'KXCPI' in names and b in ['POSTPONEMENT','SOURCE_DIVERGENCE']:branch['source_finding']='CPI terms specify a missing-data fallback and revision cutoff; effective global modification/review powers remain unresolved.'
            if b in ['DISPUTE','VOID','UNDEFINED']:branch['blocking_source']='Current effective Kalshi rulebook text/version binding unavailable: regulatory landing 429, attempted v1.29 official archive 404; for Polymarket, exact condition-level exceptional payout/collateral binding also required.'
        d['ordinary_settlement_schedule']='Contract-specific day-after-expiration convention is subject to outcome review; no unconditional capital-release bound.'
        d['review_ready_package_does_not_mean_approval_eligible']=True;write(p,d);entries.append({'relationship_id':d['relationship_id'],'file':str(p.relative_to(root)),'sha256':sha(p),'original_revision':proof['revision'],'original_artifact_sha256':proof['artifact_sha256'],'approval_status':'PENDING_HUMAN_REVIEW'})
    assert len(entries)==len(priorities)==5
    write(directory/'index.json',{'review_packages':entries,'full_contractual_proofs':0,'human_approvals':0})
    pending=read(root/'approval_manifest.pending.json')
    for entry in entries:
        review=next(r for r in pending['reviews'] if r['candidate_pair_id']==entry['relationship_id']);review['research_review_package_file']=entry['file'];review['research_review_package_sha256']=entry['sha256'];review['research_package_does_not_authorize_any_revision']=True
    assert all(r['status']=='PENDING_HUMAN_REVIEW' and not r['reviewer'] and not r['approved_revision'] for r in pending['reviews']);write(root/'approval_manifest.pending.json',pending)
    health=read(root/'feed_health_audit.json');economic=read(root/'economic_summary.json');economic['new_covered_relationship_scope']='21 SAME_VENUE; no newly selected cross-venue relationship had joint instrument coverage';economic['historical_plus_new_priced_states']=5146+economic['new_capture_priced_states'];economic['historical_gross_states_not_independent']=106;economic['historical_gross_underlying_events']=1;economic['historical_gross_episodes']=5;economic['feed_audit_reference']='feed_health_audit.json';write(root/'economic_summary.json',economic)
    ranks=table(root/'proof_review_priority.csv');already={r['relationship_id'] for r in ranks}
    ranks += [{'relationship_id':rid,'type':p['type'],'selection_cohort':'NO_USABLE_RECORDED_ECONOMICS','economic_rank':None,'gross_best':None,'fee_best':None,'budget_best':None,'priced_states':0,'intensive_review_selected':False,'approval_status':'PENDING_HUMAN_REVIEW'} for rid,p in proofs.items() if rid not in already];csvout(root/'proof_review_priority.csv',ranks)
    historical=[]
    for name in ['historical_v011','historical_v012']:
        historical += [dict(r,capture_id=name,selection_cohort='HISTORICAL_CONTROL') for r in table(OLD/'analysis'/name/'opportunity_episodes.csv')]
    csvout(root/'historical_opportunity_episodes.csv',historical)
    report_path=root/'economic_feasibility.md';text=report_path.read_text();text+='\n## 采集健康与证据边界补充\n\n'
    for connection in health['connections']:
        text+=f"- {connection['venue']}: opens={connection['connection_opens']}, unplanned disconnects={connection['unplanned_disconnects']}, disconnect-to-reopen downtime={connection['disconnect_to_reopen_downtime_sec']:.6f}s.\n"
    text+=f"\nPolymarket有{health['snapshot_reconciliation_mismatches']}次快照比较不符，触发重建并恢复；另有{health['inconclusive_comparisons']}次不可比对、{health['older_rest_snapshot_comparisons']}次较旧REST比较。具体原因、恢复与时间区间分别在feed_health_audit.json、reconciliation.csv、snapshot_recovery.csv和downtime_intervals.json。CLOB无连续公共seq，交换所层面漏消息数仍为null。\n"
    text+='\n两侧source-to-callback age存在负值，反映源钟/本机钟偏移，不能当作负网络延迟或真实订单延迟。经济回放按同机单调时钟及processing-completed可用性界执行，不用源钟负值创造提前成交；固定0/100/250/500/1000ms仍是情景，不是实测订单RTT。\n\n历史106个毛利正状态只属于同一Aliens事件的5个区间，未加进本轮新样本。合计历史与新数据35,542个报价状态、52个有报价关系；其余139个关系的经济性未知。本轮新覆盖21个关系均为同场所，不能据此宣称新短周期跨场所双taker已经被否定。\n'
    report_path.write_text(text)
    manifest=read(root/'audit_manifest.json');manifest['economic_summary_sha256']=sha(root/'economic_summary.json');manifest['feed_health_audit_sha256']=sha(root/'feed_health_audit.json');manifest['full_rule_review_package_index_sha256']=sha(directory/'index.json');manifest['finalized_at_utc']=datetime.now(timezone.utc).isoformat();manifest['no_collector_started_by_this_task']=True;manifest['original_bounded_workflow_finished']=read(capture.parent/'workflow_status.json')['status']=='BOUNDED_GENERALIZED_CAPTURE_FINISHED';manifest['native_research_binary_sha256']=sha(ROOT/'build/run_generalized_payoff_research');manifest['source_code']=[dict(r,sha256=sha(ROOT/r['file'])) for r in manifest['source_code']];manifest['outputs']=[{'file':str(p.relative_to(root)),'sha256':sha(p),'bytes':p.stat().st_size} for p in sorted(root.rglob('*')) if p.is_file() and p.name!='audit_manifest.json' and not p.name.endswith('.pyc')];write(root/'audit_manifest.json',manifest)


def reassemble_existing(root,capture):
    proofs={p['relationship_id']:p for p in map(json.loads,PROOFS.read_text().splitlines())};expected={(v,m) for v,ms in read(capture/'capture_config.json')['capture_market_ids'].items() for m in ms};selected={(m['venue'],m['market_id']) for m in read(capture/'selected_markets.json')}
    def cohort(rid):
        mids={(l['market']['venue'],l['market']['market_id']) for l in proofs[rid]['legs']}
        return 'INTENDED_NEW' if mids<=expected else ('MIXED_INTENDED_CONTROL' if mids&expected else ('FALLBACK_CONTROL' if mids<=selected else 'NOT_IN_CAPTURE'))
    quoted=[]
    for name,analysis in [('new_authenticated',root/'baseline'),('historical_v011',OLD/'analysis/historical_v011'),('historical_v012',OLD/'analysis/historical_v012')]:quoted.extend(dict(r,capture_id=name) for r in table(analysis/'portfolio_candidates.csv'))
    priorities=[r for r in table(root/'proof_review_priority.csv') if r.get('intensive_review_selected')=='True'];assert len(priorities)==5
    finalize(root,capture,proofs,quoted,{},priorities,cohort,[])

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--run-dir',type=Path,required=True);p.add_argument('--capture',type=Path,required=True);p.add_argument('--report-only',action='store_true');a=p.parse_args();(reassemble_existing if a.report_only else prepare)(a.run_dir.resolve(),a.capture.resolve())

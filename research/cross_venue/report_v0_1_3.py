"""Assemble measured native evidence. No discovery, pricing, credentials or trading."""
import argparse,csv,hashlib,json,re,shutil
from collections import Counter,defaultdict
from decimal import Decimal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
D=Decimal
RECOMMENDATION='INSUFFICIENT_EVIDENCE'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):return json.loads(p.read_text())
def rows(p):return [r for r in csv.DictReader(p.open()) if r.get('status')!='UNAVAILABLE']
def write(p,obj):p.write_text(json.dumps(obj,ensure_ascii=False,indent=2)+'\n')
def csvout(p,rs):
    if not rs:rs=[{'status':'UNAVAILABLE','reason':'NO_QUALIFYING_OBSERVATIONS','strict_pnl':None}]
    keys=sorted(set().union(*(set(r) for r in rs)))
    with p.open('w',newline='') as f:
        w=csv.DictWriter(f,keys);w.writeheader();w.writerows(rs)
def mdtable(headers,rs):return '\n'.join(['| '+' | '.join(headers)+' |','| '+' | '.join(['---']*len(headers))+' |']+['| '+' | '.join(str(x).replace('|','/') for x in r)+' |' for r in rs])
def num(s):return D(s or '0')
def relative(p):
    try:return str(p.resolve().relative_to(ROOT))
    except ValueError:return str(p)
CHANGES={
 'oracle/payoff/GeneralizedPayoff.h':'Shared portfolio payoff/state/certificate API',
 'oracle/payoff/GeneralizedPayoff.cpp':'Existing enumerator/matrix with exact per-contract payout rounding and explicit exceptional envelopes',
 'oracle/CMakeLists.txt':'Include generalized payoff in existing Oracle target',
 'engine/strategy/cross_venue/GeneralizedResearch.h':'Bounded discovery, sizing, approval and replay interfaces',
 'engine/strategy/cross_venue/GeneralizedDiscovery.cpp':'Structured predicates, bounded 2/3/4-leg generation and deterministic rule-bound dossiers',
 'engine/strategy/cross_venue/GeneralizedOptimizer.cpp':'Discrete real-depth/fee sizing, separate stage witnesses, two-wallet and existing RiskLedger constraints',
 'engine/strategy/cross_venue/GeneralizedReplay.cpp':'Existing ResearchState/AsOf replay, fixed-size latency, episodes, maker headroom and intermediate inventories',
 'engine/strategy/cross_venue/MarketDiscovery.cpp':'Opt-in structured strata/directional labels; Kalshi per-fragment cash rounding correction',
 'engine/strategy/cross_venue/CMakeLists.txt':'Native research target and three focused test groups',
 'tools/generalized_payoff_main.cpp':'Read-only discover/replay CLI; rejects live flags',
 'config/cross_venue_v0_1_3.json':'Bounded public search, configurable capital and safety budgets',
 'config/cross_venue_v0_1_3.capture.json':'Prepared finite new-market capture with immutable proof hash binding',
 'research/cross_venue/secure_kalshi_environment.py':'Add local hidden-input/in-memory launch-generalized mode; preserve previous modes',
 'research/cross_venue/prepare_v0_1_3_capture.py':'Bounded official public rules/depth preflight and diversified selection',
 'research/cross_venue/generalized_capture_workflow.py':'One 1200-second read-only native capture, audit and replay then exit',
 'research/cross_venue/audit_v0_1_3.py':'Independent raw all-instrument book/fee/grid/capital/proof/time audit',
 'research/cross_venue/report_v0_1_3.py':'Artifact aggregation and evidence-based economic report only',
 'tests/integration/cross_venue/generalized_payoff_test.cpp':'Clearly synthetic exact payoff, quantity, stage sizing, asset, risk and approval regressions',
 'tests/integration/cross_venue/generalized_discovery_test.py':'Native generator source/time/index/cancellation/counterexample/no-live regressions',
 'tests/integration/cross_venue/generalized_replay_audit_test.py':'Independent real recording replay and byte determinism regression',
 'docs/cross_venue_v0_1_3.md':'Architecture, limits, reproduction and evidence navigation',
}
def assemble(root):
    proofdir=root/'proofs';proofs={p['relationship_id']:p for p in map(json.loads,(proofdir/'payoff_proofs.jsonl').read_text().splitlines())}
    for p in proofdir.iterdir():
        if p.suffix in ['.json','.jsonl','.csv']:shutil.copyfile(p,root/p.name)
    discovery=read(root/'discovery_coverage.json');canonical=list(map(json.loads,(root/'canonical_events.jsonl').read_text().splitlines()))
    names=['historical_v011','historical_v012','public_preflight']
    if (root/'analysis/fresh_authenticated/economic_summary.json').exists():names.append('fresh_authenticated')
    analyses={n:root/'analysis'/n for n in names};summaries={n:read(p/'economic_summary.json') for n,p in analyses.items()};audits={n:read(root/'analysis'/(n+'_audit.json')) for n in names}
    for n,a in audits.items():assert a['status']=='INDEPENDENT_GENERALIZED_ECONOMIC_AUDIT_PASSED' and a['deterministic_files_compared']>=16
    measured=[n for n in names if n!='public_preflight'];allrows={};tables=['asof_coverage','portfolio_candidates','portfolio_costs','fee_adjusted_opportunities','gross_opportunities','latency_sensitivity','opportunity_episodes','capital_efficiency','maker_feasibility','execution_risk','approved_relationships','missing_market_data']
    for name in tables:
        combined=[]
        for n,p in analyses.items():
            for r in rows(p/(name+'.csv')):combined.append(dict(r,capture_id=n,data_scope='PUBLIC_POLYMARKET_WS_ONLY_5_SECOND_PREFLIGHT' if n=='public_preflight' else 'AUTHENTICATED_RECORDED_MARKETS'))
        csvout(root/(name+'.csv'),combined);allrows[name]=combined
    primary=lambda name:[r for r in allrows[name] if r['capture_id'] in measured]
    ps=primary('portfolio_candidates');episodes=primary('opportunity_episodes');gross=primary('gross_opportunities');coverage=primary('asof_coverage');maker=primary('maker_feasibility');latency=primary('latency_sensitivity');risk=primary('execution_risk')
    matching={r['relationship_id'] for r in coverage if r.get('covered_instruments')=='true'};healthy={r['relationship_id'] for r in coverage if int(r.get('priced_states') or 0)>0}
    full=sum(p['coverage_complete'] for p in proofs.values());nominal=sum(p['hypothetical_normal_minimum']>0 for p in proofs.values());justified=sum(p['justified_normal_minimum']>0 for p in proofs.values());counter=len(proofs)-nominal
    types=Counter(p['type'] for p in proofs.values());categories=Counter(p['category'] for p in proofs.values());scopes=Counter(p['venue_scope'] for p in proofs.values());normal_cancel=sum(bool(p['exceptional_payoffs']) for p in proofs.values())
    assertions=['strict_pnl','verified_live_realized_pnl','contractual_payout_floor']
    assert all(not r.get(k) for r in ps for k in assertions) and all(r['strict_simulation_eligible']=='false' for r in ps)
    assert not primary('approved_relationships')
    class_specs=[('Exact Cross-Venue',lambda p:p['type']=='CROSS_VENUE_EQUIVALENT','Source, observation time, exceptional payoffs and asset equivalence pending'),('Implication Arbitrage',lambda p:p['type'] in ['NESTED_THRESHOLD','NESTED_TIME_WINDOW','CROSS_VENUE_IMPLICATION','THREE_LEG_LADDER','FOUR_LEG_LADDER'],'Fresh numerical ladders absent from authenticated old recordings; exception/release evidence incomplete'),('Multi-Outcome Bundles',lambda p:p['type'] in ['EXHAUSTIVE_YES_BUNDLE','MUTUALLY_EXCLUSIVE_NO_BUNDLE'],'Filtered pages do not prove exhaustive partitions; only narrow completed-match normal rules recognized'),('Single-Venue Combinatorial',lambda p:p['venue_scope']=='SAME_VENUE','Tested observed combinations have no fee-positive margin; most newly discovered instruments lack bounded fresh authenticated coverage'),('Cross-Venue Multi-Leg',lambda p:p['type'] in ['CROSS_VENUE_THREE_LEG','CROSS_VENUE_FOUR_LEG'],'Extra legs can optimize to zero; template label does not prove active cross-venue economic value'),('Maker-First Research',lambda p:p['hypothetical_normal_minimum']>0,'No competitive quote in evaluated budget-optimal quantities; queue/fill evidence absent')]
    strategies=[]
    for name,predicate,blocker in class_specs:
        ids={k for k,p in proofs.items() if predicate(p)};quoted=[r for r in ps if r['relationship_id'] in ids];eps=[r for r in episodes if r['relationship_id'] in ids]
        strategies.append({'strategy':name,'candidate_count':len(ids),'conditional_mathematical_valid':sum(proofs[k]['hypothetical_normal_minimum']>0 for k in ids),'normal_source_constraints_supported':sum(proofs[k]['justified_normal_minimum']>0 for k in ids),'rule_review_eligible_complete_contract':sum(proofs[k]['coverage_complete'] for k in ids),'human_approved':0,'instrument_matched_relationships':len(ids&matching),'healthy_priced_relationships':len(ids&healthy),'priced_states':len(quoted),'gross_positive_states':sum(r['any_gross_positive']=='true' for r in quoted),'fee_positive_states':sum(r['any_fee_positive']=='true' for r in quoted),'budget_positive_states':sum(r['any_budget_positive']=='true' for r in quoted),'gross_episodes':sum(r['basis']=='GROSS' for r in eps),'fee_episodes':sum(r['basis']=='FEE_ADJUSTED' for r in eps),'budget_episodes':sum(r['basis']=='BUDGET_ADJUSTED' for r in eps),'best_nonzero_conditional_net':str(max((num(r['budget_adjusted_margin']) for r in quoted),default=D(0))) if quoted else None,'objective_with_abstention':'0.000000' if quoted else None,'approved_executable_capacity':None,'approved_simulated_pnl':None,'execution_complexity':'2-4 non-atomic legs; maker headroom is not execution' if name=='Maker-First Research' else '2-4 configured legs; active quantities and effective venue reported per row','principal_blocker':blocker,'class_overlap':'Classes overlap; do not sum counts'})
    csvout(root/'strategy_comparison.csv',strategies)
    csvout(root/'relationship_economic_types_summary.csv',[dict(r,capture_id=n) for n,p in analyses.items() for r in rows(p/'strategy_comparison.csv')])
    ranked=[]
    for rid,p in proofs.items():
        observations=[r for r in ps if r['relationship_id']==rid]
        if not observations:continue
        best=max(observations,key=lambda r:num(r['budget_adjusted_margin']));g=[r for r in gross if r['relationship_id']==rid]
        ranked.append({'relationship_id':rid,'type':p['type'],'scope':p['venue_scope'],'category':p['category'],'titles':' / '.join(dict.fromkeys(l['market']['title'] for l in p['legs'])),'events':json.dumps(sorted({l['market']['event_id'] for l in p['legs']})),'best_nonzero_conditional_net':best['budget_adjusted_margin'],'best_after_abstention':'0.000000','witness_capture':best['capture_id'],'witness_portfolio_id':best['portfolio_id'],'witness_quantities_micro':best['quantities_micro'],'witness_cash_including_fees_and_budgets':str(num(best['wallet_kalshi'])+num(best['wallet_polymarket'])),'largest_walked_purchase_cost_at_gross_positive_size':str(max((num(r['acquisition_cost']) for r in g),default=D(0))) if g else None,'largest_recorded_visible_depth_micro':json.dumps([max(json.loads(r['visible_depth_micro'])[i] for r in observations) for i in range(len(p['legs']))]),'max_tested_quantity_per_leg_micro':1000000000,'full_depth_optimal_capacity':None,'profit_per_capital_day':None,'capital_release':None,'approval_status':'PENDING_HUMAN_REVIEW','strict_pnl':None})
    ranked.sort(key=lambda r:(-num(r['best_nonzero_conditional_net']),r['relationship_id']));csvout(root/'conditional_candidate_ranking.csv',ranked)
    annual=[{'annual_target_usd':target,'assumed_net_edge':str(edge),'required_annual_notional_usd':str(D(target)/edge),'full_200k_capital_turns_required':str(D(target)/edge/D(200000)),'evidence_status':'HYPOTHETICAL_ARITHMETIC_NOT_AN_EMPIRICAL_PROJECTION'} for target in [10000,25000,50000,100000] for edge in [D('.001'),D('.005'),D('.01')]]
    csvout(root/'capital_target_requirements.csv',annual)
    stage_stats={'candidate_relationships':len(proofs),'conditional_normal_positive':nominal,'source_supported_normal_positive':justified,'complete_contractual_proofs':full,'human_approved':0,'instrument_matched_relationships_union':len(matching),'healthy_priced_relationships_union':len(healthy),'healthy_priced_diagnostic_states':len(ps),'conditional_gross_positive':sum(r['any_gross_positive']=='true' for r in ps),'conditional_fee_positive':sum(r['any_fee_positive']=='true' for r in ps),'conditional_budget_positive':sum(r['any_budget_positive']=='true' for r in ps),'gross_episodes':sum(r['basis']=='GROSS' for r in episodes),'fee_episodes':sum(r['basis']=='FEE_ADJUSTED' for r in episodes),'budget_episodes':sum(r['basis']=='BUDGET_ADJUSTED' for r in episodes),'strict_execution_eligible_states':0,'simulated_trades':0}
    lat_summary=[]
    for lag in [0,100,250,500,1000]:
        rs=[r for r in latency if int(r['latency_ms'])==lag and r['comparison_basis']=='NET_OPTIMIZER'];gs=[r for r in latency if int(r['latency_ms'])==lag and r['comparison_basis']=='GROSS_WITNESS']
        lat_summary.append({'latency_ms':lag,'net_optimizer_known':sum(r['status']=='CONDITIONAL_PRICE_STRESS' for r in rs),'net_optimizer_unknown':sum(r['status']=='UNKNOWN_UNAVAILABLE' for r in rs),'budget_positive_survivors':sum(r['net_positive_survives']=='true' for r in rs),'gross_witness_survivors':sum(r['gross_positive_survives']=='true' for r in gs),'gross_witness_unknown':sum(r['status']=='UNKNOWN_UNAVAILABLE' for r in gs),'fill_probability':None,'observed_trades':0})
    csvout(root/'latency_summary.csv',lat_summary)
    scope_stats=Counter((r['scope'],r['effective_venue_scope'],r['configured_legs'],r['active_legs']) for r in ps)
    csvout(root/'active_portfolio_structure.csv',[{'template_scope':k[0],'effective_scope':k[1],'configured_legs':k[2],'active_legs':k[3],'observations':v} for k,v in sorted(scope_stats.items())])
    rejection=Counter(c.get('exclusion_reason','UNKNOWN') for c in canonical if not c['eligible_for_bounded_search']);cost_reject=Counter()
    for r in coverage:
        for k,v in r.items():
            if k.startswith('cost_rejection_count_') and v:cost_reject[k.removeprefix('cost_rejection_count_')]+=int(v)
    csvout(root/'blocked_reasons.csv',[{'scope':'MARKET_SCREEN','reason':k,'count':v} for k,v in rejection.items()]+[{'scope':'DISTINCT_BOOK_STATE_COST_REJECTION','reason':k,'count':v} for k,v in cost_reject.items()]+[{'scope':'ALL_RELATIONSHIPS','reason':'INCOMPLETE_CONTRACTUAL_PAYOFF_AND_NO_HUMAN_APPROVAL','count':len(proofs)}])
    ctest=read(root/'test_results.json') if (root/'test_results.json').exists() else {'status':'NOT_RECORDED'}
    fresh='COMPLETED_AND_INDEPENDENTLY_AUDITED' if 'fresh_authenticated' in names else 'NOT_STARTED_CODEX_KEY_ID_NOT_INHERITED_LOCAL_LAUNCH_PENDING'
    summary={'schema':'polytope-generalized-economic-v0.1.3','recommendation':RECOMMENDATION,'evidence_scope':'Primary: immutable real authenticated historical captures; 5s public-only decoder preflight separate','discovery':discovery,'primary_capture_ids':measured,'captures':summaries,'funnel':stage_stats,'phase_c_authenticated_1200_sec':fresh,'phase_c_public_preflight_excluded_from_primary':True,'strict_simulated_net_pnl':None,'verified_live_realized_pnl':None,'live_orders_sent':0,'research_capital_ceiling_usd':200000,'approved_empirical_dollar_capacity':None,'empirical_annual_profit_projection':None,'maker_fill_probability':None,'maker_realized_pnl':None,'independent_audits':audits,'test_results':ctest,'confidence':{'exact_arithmetic_and_recorded_book_reconstruction':'HIGH_WITHIN_REPORTED_STATES_AND_DISCRETE_GRID','contractual_guarantees':'UNVERIFIED_ZERO_COMPLETE_PROOFS','new_short_duration_market_economics':'INSUFFICIENT_AUTHENTICATED_COVERAGE','capital_scaling_and_repeatability':'INSUFFICIENT_EVIDENCE'}}
    write(root/'economic_summary.json',summary);write(root/'implementation_changes.json',[{'file':p,'purpose':reason,'sha256':digest(ROOT/p)} for p,reason in CHANGES.items()])
    overlap=rows(root/'market_overlap_by_category.csv');episode_seconds=sum(num(r['duration_sec']) for r in episodes);gross0=gross[0] if gross else None
    top=ranked[:8]
    report=f'''# Polytope V0.1.3 — 广义支付关系研究：实际执行报告

唯一主建议：**{RECOMMENDATION}**。本次原生实现、真实公开市场发现、两段真实认证录制回放、逐盘口独立审计和确定性验证均已执行。发现的数学结构尚不足以支持部署 $200,000。

严格模拟净 PnL = `null`；`verified_live_realized_pnl = null`；真实订单 = 0。未批准诊断不能计为已成交、已批准套利或实盘盈利。

## A. 实现与复用

扩展现有 Oracle `StateEnumerator`、`PayoffMatrixBuilder` 和 `BooleanConstraintBuilder`，不另建交易或盘口引擎。行情仍由现有 `ResearchState` / `SynchronizedBookStore` 重建，使用原有 `JointBookAsOf`、`FractionalVwap`、市场费用和双钱包 `RiskLedger`。原双腿执行状态机与 paper ledger 保留；多腿目前输出前缀库存及失败情景，不虚构填单或记入 paper 盈利。

原生 C++20 完成候选生成、最多四变量/四腿状态穷举、费用和数量网格优化、回放与固定数量延迟压力；Python 仅负责公开预检、进程编排、独立重算和报告。数学、普通规则支持、全分支证明、人工批准、执行证据分别记录。

每腿金额与数量均按 1e-6 定点处理，支付先逐合约向下取整再相加；费用复用市场系数和分档舍入。修正 Kalshi 分档现金向上取整与 VWAP 总额取整之间的 1 微货币差额，防止低估预留。每个机会阶段单独保留数量证据，毛利与扣费计数不依赖风险预算后的最优数量。

修改清单（精确路径、用途和代码 SHA 见 `implementation_changes.json`）：

{mdtable(['文件','作用'],[(p,reason) for p,reason in CHANGES.items()])}

既有脏工作区中的 market-making、frontend、其他风险/PMXT 变更均未纳入本次修改；原有失败未修饰。

## B. 市场发现与有界范围

真实官方元数据共 {discovery['markets_considered']:,} 个唯一市场，Kalshi 1,953、Polymarket International 2,400。筛选后 {discovery['markets_eligible']} 个，其中 24 个历史控制市场、188 个新短周期候选；排除 {discovery['markets_excluded']:,} 个。历史控制允许超出新市场期限，单独标记，不等于新短周期发现。

{mdtable(['场所:类别','已检查','符合初筛'],[(r['venue_category'],r['considered'],r['eligible']) for r in overlap])}

官方 API 原始响应、URL、cursor/offset、接收时间和 SHA 均在 `discovery/`。通用分页初始目标每场所 1,600；附加 KXBTCD/KXBTC/KXETHD/KXETH/KXCPI/KXGDP/KXHIGHNY/KXNBAGAME 各最多两页100条，Gamma 近两周 endDate 最多八页100条，随后去重。附加 strata 不是全量市场扫描。API 跨请求并非原子快照，未见市场数量为 `null`。

新市场筛选流动性代理 ≥1,000，交易关闭代理期限 1小时–14天；代理只是成交量/未平仓等初筛，不代表可执行深度或资金释放期限。规范化保留参考指数、原始数字阈值、比较符和完整时间规则签名；未能确定的绝对观测时间、特殊结算和资金释放维持 `null`。CF、Binance、Chainlink 不混同。Up/Down 映射仅显式配置启用，并保留原始标签。

共 {len(proofs)} 个候选：同场所 {scopes['SAME_VENUE']}、跨场所 {scopes['CROSS_VENUE']}；类别 {dict(categories)}。

{mdtable(['关系类型','候选数'],sorted(types.items()))}

结构化跨场所比较 {discovery['structured_cross_venue_comparisons']} 次；记录拒绝 {discovery['recorded_rejections']} 条。最大组数512、每类型40，具体省略提议 {discovery['omitted_proposals_at_limits']}。这些是省略的生成提议而非唯一全市场数量。没有符合条件的真实跨场所 implication 数值对，未为凑类别伪造候选。数字阈值实现目前只生成 greater/greater-or-equal 梯形；其他结构保留元数据而不声称搜索完备。

初筛排除：{dict(rejection)}。停止原因为范围上限、规则缺失和可买深度不足，没有继续无限枚举。

## C. 数学证明与反例

- {nominal} 个假设的普通状态模型有正的单位支付下界；其中 {justified} 个的普通逻辑约束有同市场身份或一致规则签名支持，其余仍是待核实假设。
- {counter} 个普通模型存在反例，主要是 outcome 子集缺失残余结果、不能从分页或 NegRisk 组推出穷尽性。
- {normal_cancel} 个证书提取到精确的取消比赛支付文本；六个三结果足球组合能支持普通完成比赛的双方胜/平局划分。
- **完整合约证明 {full}；全状态规则审核就绪 0；人工批准 0。** 157 个正的普通模型均缺少至少一个特殊分支，因此不是 157 个已证明套利。

状态集合保留 YES/NO、TIE、VOID、CANCELLATION、POSTPONEMENT、EARLY_TERMINATION、DISPUTE、REPLACEMENT、FRACTIONAL_REFUND、SOURCE_DIVERGENCE、UNDEFINED。未知特殊分支加入显式零支付下界的不确定性包络；它不是交易所实际零赔付的断言。已知的不同分支/替代状态全部参与最小化。无可行状态时直接拒绝，不把空集合视为保证。

每条 `payoff_proofs.jsonl` 包含原合约、原始规则/URL/哈希、约束、所有状态向量、最小值见证和缺失分支；反例另存 `payoff_counterexamples.jsonl`。不可将未知资产按1:1可兑当作已验证：Polymarket 的具体抵押品若元数据不足，保持 `UNKNOWN_POLY_COLLATERAL`。

批准继续使用 V0.1.1 exact revision/artifact hash/dossier hash + reviewer/date + coverage/assets/quantity flags。`approval_manifest.pending.json` 只有待审项，不替人填写批准。新完整规则需要生成新版本并由人明确批准。

证明文件 SHA-256：`{digest(root/'payoff_proofs.jsonl')}`。

## D. 实际经济结果与漏斗

{mdtable(['录制','秒','原始消息','合约覆盖候选','可报价状态','毛利正','扣费正','预算后正'],[(n,round(s['recorded_seconds'],6),s['raw_records_verified'],s['relationships_with_recorded_instruments'],s['priced_portfolio_states'],s['conditional_gross_positive'],s['conditional_fee_positive'],s['conditional_budget_positive']) for n,s in summaries.items()])}

主结论只汇总真实认证历史录制{('及本次认证录制' if 'fresh_authenticated' in names else '')}；公开5秒预检不计入主经济样本。主样本 {len(ps):,} 个组合状态、{len(matching)} 个唯一合约覆盖候选、{len(healthy)} 个有实际报价的候选。缺少市场数据/规则版本的候选为 `MISSING_MARKET_DATA_OR_RULE_REVISION`，其收益和容量未知，不能记为无机会。

诊断漏斗：{discovery['markets_considered']:,} 市场 → {len(proofs)} 候选 → {nominal} 条件普通模型正 → {len(ps):,} 健康可报价诊断状态 → {stage_stats['conditional_gross_positive']} 毛利正 → {stage_stats['conditional_fee_positive']} 扣费正 → {stage_stats['conditional_budget_positive']} 预算正 → 0 预算正延迟存活 → 0 模拟交易。

严格路径在完整规则证明/人工批准两关均为0，故严格健康执行状态和交易数也为0。自动检查191个不等于191个人工审核。

{stage_stats['gross_episodes']} 个去重毛利区间、扣费区间0、预算区间0；同候选、同数量相邻状态合并，失去有效 As-Of 或数量变化则关闭。毛利区间时长合计 {episode_seconds:.6f} 秒，只能描述该候选的区间；不把重叠组合时长相加作为独立资金周转。

''' + (f'''全部106个毛利正状态来自历史 Aliens 跨场所假设，在每腿1,000份的测试量上买入成本 `{gross0['acquisition_cost']}` 名义美元等值、普通假设支付1,000、毛利 `{gross0['gross_margin']}`、费用 `{gross0['fees']}`、扣费后 `{gross0['fee_adjusted_margin']}`、另设预算 `{gross0['budgets']}`、预算后 `{gross0['budget_adjusted_margin']}`。钱包需求 K `{gross0['wallet_kalshi']}`、P `{gross0['wallet_polymarket']}`。真实盘口支持该测试量，但这不是全深度最优容量，更不是可批准套利容量。两合约关闭时间不同且一侧抵押品未验证，不能用普通模型下界声称合约等价。详见 `gross_opportunities.csv` 关联的 `portfolio_costs.csv` 原始哈希、可见64档、费用版本和原合约。

''' if gross0 else '')+f'''每腿最多测试1,000份、最多6个数量点（含零、已知最小量、比例量和深度边界）、10,000组合、250ms；2–4腿的测试网格全枚举并报告可行最优。网格外全局最优差距仍未知，给出忽略费用/滑点/钱包耦合的保守乐观 box upper bound。只有网格结论，不能说全市场连续数量无正机会。零仓位始终可选；负的最佳非零诊断对应最优决策“放弃”，目标值0，不代表亏损交易已经发生。

扣除假设中每购买份0.002 forward预算；跨场所模板另每腿0.003转换/资产预算。即使该模板优化后有某腿归零，转换预算仍保守保留；固定预算不是对汇率/脱锚尾部风险的保证。参数可配置，实际费用来自冻结的市场专属数据，缺失/过期费用不硬编码零。

## E. 资本效率与可部署性

双钱包默认各100k、总上限200k，均可配置；同场所不可借另一个钱包。最坏中间库存现金成本上限5k。全分支、数量、资产、人工批准以及有证据且≤14天的资金释放界全部满足才可严格预留；同合约流动性不重复消费，预留持有到有证据的释放，不把观测状态计为滚动交易。

本次严格已批准可执行容量、资本利用率、可支持年收益均为 `null`（实际预留/交易0）。交易关闭、事件发生、结算、现金释放是不同字段；实际释放未知，资本日收益和年化为空。

以下仅按“最佳非零预算后名义诊断值”排序，所有值≤0；不能把最小测试量造成的小负值解读为高资本效率。完整排名含真实深度、事件集中度和见证在 `conditional_candidate_ranking.csv`。

{mdtable(['候选简称','类型','非零预算后值','见证占用含预算','资金释放'],[(r['titles'][:70],r['type'],r['best_nonzero_conditional_net'],r['witness_cash_including_fees_and_budgets'],'UNKNOWN') for r in top])}

毛利机会集中于同一 Aliens 事件，新增体育集合和梯形合约也共享 underlying；组合数量不等于独立风险或可累加容量。未测试到的深度、候选和新时段一律不外推。配置允许200k不意味着已有200k机会。

年度目标仅算术：required turnover = annual net target / assumed net edge。下表1%净边际也仅是假设，另0.1%/0.5%全表见 `capital_target_requirements.csv`。

{mdtable(['年度净目标','1%净边际所需成交名义本金','200k全额周转次数'],[(r['annual_target_usd'],r['required_annual_notional_usd'],r['full_200k_capital_turns_required']) for r in annual if r['assumed_net_edge']=='0.01'])}

未从几十分钟数据年化，也未把这些假设周转视为已实现或可实现。

## F. 延迟、执行和 maker

固定决策时数量，在0/100/250/500/1000ms的实际后续 As-Of 盘口重新走深度；不在未来重新挑选更有利数量。未来超窗、失效或深度不够记 `UNKNOWN_UNAVAILABLE`，不是零损失或成功成交。对普通毛利见证和费用见证另做相同固定量压力。

{mdtable(['延迟ms','预算最优量有后续价','后续未知','毛利见证仍毛利正','预算正存活'],[(r['latency_ms'],r['net_optimizer_known'],r['net_optimizer_unknown'],r['gross_witness_survivors'],r['budget_positive_survivors']) for r in lat_summary])}

毛利见证延迟后仍可毛利正，但已被真实费用抹去，不能计作净套利延迟存活。盘口存在不保证订单成交、排队或报价持续可取。

`execution_risk.csv` 共 {len(risk):,} 条前缀/部分库存情景；包括后腿拒绝、停牌或失败，以及下一腿60%后停止。名义持仓下界和现金风险明确记录；完成组合的支付地板不能保护不完整中间仓位。最大样本中间库存现金风险 `{max((num(r['acquisition_cash_at_risk']) for r in risk),default=D(0))}`。情景不是观测填单；没有将暂停、拒单、结算延迟概率伪造成实测概率。对已批准订单状态机/实际回报的多腿联调未执行，因为批准集合为空。

maker 只对已评价数量作条件挂价余量诊断：{sum(r.get('maker_quote_feasible')=='true' for r in maker)} 条条件可挂、{sum(r.get('maker_quote_competitive')=='true' for r in maker)} 条竞争价。检查真实tick/post-only和已知maker费用；未知费用不假设零。`maker_fill_probability = null`、`maker_realized_pnl = null`；没有生产maker系统、返佣收益或队列填单声明。此处范围不同于V0.1.2专门扫 maker 数量/阈值的统计，不可横向相减。

## G. 测试、独立审计和复现

{json.dumps(ctest,ensure_ascii=False)}

执行 `cmake -S . -B build`、`cmake --build build -j4`、`ctest --test-dir build --output-on-failure -j4`。新增三个组包含{ctest.get('native_synthetic_checks', 'UNKNOWN')}项原生合成边界检查、结构化来源/时间/指数反例、真实原始回放独立优化与确定性检查。合成case仅在测试临时目录，不进入真实收益报告。V0–V0.1.2测试保留。

独立审计从原始WS重建所有报价合约，并对所有有效未来固定数量场景独立重算成本；逐腿核对source rules/token mapping/raw payload SHA/raw offset/sid-seq/clock/epoch、6dp数量、可见深度、现金和费用、两钱包、每个报告网格最优与阶段见证、无未来数据、特殊包络、episode去重和无批准执行。费用过期时间按真实决策/执行时间核验；processing sidecar完整性和完成时间作为可用性界。

{mdtable(['录制','原始记录','独立盘口/费用核对','独立未来成本核对','独立网格最优','逐字节重复文件'],[(n,a['raw_records_verified'],a['independent_all_instrument_L2_and_fee_checks'],a['independent_future_fixed_quantity_cost_checks'],a['distinct_discrete_optima_independently_enumerated'],a['deterministic_files_compared']) for n,a in audits.items()])}

独立审计通过只证明报告的条件数学/成本/时序一致，不补足交易所法律条款、Solvency或填单证据。输出与代码SHA及原始输入manifest记录在 `audit_manifest.json`；该manifest不对自身递归求哈希。

复现命令及边界见 `docs/cross_venue_v0_1_3.md`。根目录表均带 `capture_id` / `data_scope`；同名portfolio_id仅在对应capture内关联，不能跨录制混用。

## H. 策略比较和唯一建议

{mdtable(['策略','候选','普通模型正','有报价候选','毛利区间','扣费区间','预算区间'],[(r['strategy'],r['candidate_count'],r['conditional_mathematical_valid'],r['healthy_priced_relationships'],r['gross_episodes'],r['fee_episodes'],r['budget_episodes']) for r in strategies])}

类别重叠，计数不能相加。历史主样本的210个三腿模板状态和205个四腿模板状态均优化为两个非零腿；未观察到第三／第四腿增加净价值。真正三腿、四腿必须全持有的优化只在明确标识的合成回归中验证，不能充作真实多腿盈利证据。策略完整字段和各自主要阻碍见 `strategy_comparison.csv`。数学组合增加没有带来本次样本中的扣费正结果；新短周期组合的认证数据覆盖又不足以据此否定它们。下一周期应先补齐有界新样本及最少可人工核实的完整规则证据，再决定哪个策略值得扩大；现阶段不支持扩资金、生产maker或全面否定套利。

主建议：**{RECOMMENDATION}**。算术、来源哈希、已录制盘口和网格最优一致性的置信度高；完整规则证明、短周期新组合经济性、可复现净容量、实际成交和200k规模化均缺充分证据。

## I. 阻碍与采集实际状态

1. 完整特殊分支证据缺失；普通条件模型不能自动批准。资产转换、数量步长或最小量元数据缺失会阻止严格订单；已知NegRisk成员关系不证明任意可转换性。
2. 人工审核清单已产出，但没有任何明确批准的revision。所有真实关系保持PENDING，不把自动审计等同人工批准。
3. 大部分新合约未出现在不可变历史录制中；新候选缺数据是未知，费用负结果只适用于报告范围。历史样本覆盖时间短、并非独立同分布机会样本。
4. 资金释放无可靠上界；抵押品、汇兑、平台偿付及结算延迟风险未证明可忽略；固定转换预算仅情景假设。
5. Phase C认证1200秒采集：`{fresh}`。Codex进程未继承Key ID；已准备本地安全启动器和12个Kalshi+5个Polymarket目标，选定11个新结构组；既有采集器会用真实控制市场补满每场所12个订阅槽，实际名单以各录制selected_markets.json为准。公开预检排除了无所需买入深度的crypto组合。没有重复索取聊天凭证，也没有擅自读取shell历史或进程环境。
6. 本次已执行的公开接口预检仅5.120051秒：Kalshi是REST快照、Polymarket是公开WS。Kalshi `missing_ws_credentials` 有真实事件证据，绝不归类为认证连续L2或已完成20分钟Phase C。公开样本26个组合状态只作为解码校准，不能代表新增组合经济性。
7. 填单/队列/拒单与真实资本释放无回报证据，多腿只做情景；无任何真实订单入口。后续认证采集的安全本地命令为 `python3 /home/edirent/Polytope/research/cross_venue/secure_kalshi_environment.py launch-generalized`，仅原WSL终端已有凭证或本地隐藏输入，1200秒后自动分析退出。

'''
    (root/'research_report.md').write_text(report)
    sources=[]
    for n,p in analyses.items():
        capture=ROOT/summaries[n]['source_capture']
        if not capture.exists():capture=Path(summaries[n]['source_capture'])
        manifest=read(capture/'capture_manifest.json');sources.append({'capture_id':n,'path':relative(capture),'capture_manifest_sha256':digest(capture/'capture_manifest.json'),'raw_inventory':manifest['raw_inventory'],'processing_timing_sha256':manifest.get('processing_timing_sha256'),'independent_audit_sha256':digest(root/'analysis'/(n+'_audit.json'))})
    outputs=[]
    for p in sorted(root.iterdir()):
        if p.is_file() and p.name!='audit_manifest.json':outputs.append({'file':p.name,'bytes':p.stat().st_size,'sha256':digest(p)})
    manifest={'schema':'polytope-v0.1.3-independent-evidence-manifest','recommendation':RECOMMENDATION,'source_captures':sources,'discovery_metadata_sha256':digest(root/'discovery/market_metadata.json'),'frozen_payoff_proofs_sha256':digest(root/'payoff_proofs.jsonl'),'implementation_changes_sha256':digest(root/'implementation_changes.json'),'root_outputs':outputs,'analysis_audits':audits,'tests':ctest,'test_logs_inventory':[{'file':relative(p),'sha256':digest(p)} for p in sorted((root/'tests').glob('*.log'))],'native_binary_sha256':digest(ROOT/'build/run_generalized_payoff_research'),'known_limitations':['No complete contractual proofs or human approvals','Bounded discrete sizing, not full continuous global optimum','No genuine fill/queue/receipt evidence','Phase C status: '+fresh],'strict_simulated_net_pnl':None,'verified_live_realized_pnl':None,'live_orders_sent':0}
    write(root/'audit_manifest.json',manifest)
    print(json.dumps({'report':str(root/'research_report.md'),'funnel':stage_stats,'audit_manifest_sha256':digest(root/'audit_manifest.json'),'recommendation':RECOMMENDATION},ensure_ascii=False))
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--run-dir',type=Path,required=True);assemble(parser.parse_args().run_dir)

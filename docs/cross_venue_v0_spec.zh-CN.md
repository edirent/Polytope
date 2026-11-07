Polytope Cross-Venue V0 — 实施规格与 PnL 审计演示
**状态：**可执行的离线确定性验证包；不是已接入 Kalshi / Polymarket 的生产程序，也不含任何真实成交。所有 fixture 均为合成测试数据。
一、最终目标与非目标
V0 只处理两个已被批准、在通常结算状态下互补的二元合约：在平台 A 买 canonical YES 与平台 B 买 canonical NO（或反过来）。V0 的目的，是建立从 L2 Book → net edge → intent → risk → execution event → inventory → PnL audit 的完整可审计链路。禁止把盘口信号当成实现收益。

V0 实盘开关强制默认 false。在没有明确账户/场所交易资格、独立规则比对、真实回报对账、连续 shadow 验收前，不能开真实订单。

非目标：Maker-First、收益奖励、NegRisk 多结果转换、全市场组合优化、可跨 venue 原子成交的幻想、BTC 15m 研究策略融合。
二、运行演示
Python 3.10+，无需第三方依赖。`cd` 到解压目录：
python3 -m unittest discover -s tests -v
python3 src/scan.py --input fixtures/synthetic_books.json --out out/opportunities.csv
python3 src/pnl_audit.py --input fixtures/synthetic_fills.json --out out/pnl_audit.csv
两个 CSV 分别展示：(1) 同步盘口、深度与预算费用后的候选信号；(2) 按模拟成交回报重算的终局最差 PnL（若互补规则有效）。
输入文件 SHA-256 被写入 PnL CSV，仅能证明本地审计输入是否被更改；它不能认证交易所数据来源。真实交易需要通过私有 API 重拉订单/成交/结算记录并核对到账及交易费用。

测试基准（全部是合成数据）
场景	显示盘口	收到的模拟成交	两种结果中的最低总 PnL	结果解释
matched_100	YES 47¢、NO 48¢	双边各 100 份，费用 $1.75 + $1.74	+$1.51	两腿完整覆盖；正常结算不依赖输赢
partial_hedge_60	无关	YES 100 份，NO 只有 60 份	−$18.60	不能把 60 份配对部分收益当整笔收益
hedge_slippage	无关	YES 47¢、NO 最终 52¢，各 100 份	−$2.49	对冲成交但价格变差，锁定亏损
signal_no_fills	可能曾有信号	没有成交	$0.00	信号本身不产生 PnL


matched_100 的可复算公式：
K YES   100 × $0.47 = $47.00
P NO    100 × $0.48 = $48.00
费用                   $ 3.49
总成本                 $98.49
YES 发生总赔付        $100.00
NO 发生总赔付         $100.00
结算下限 PnL          $ 1.51
此结果是数学/代码一致性验证；不是实际市场中赚到 $1.51。测试中的 illustrative_settled_pnl_usd 只是人为设定结果的复算；`verified_live_realized_pnl_usd` 故意保持空值。
三、代码加入 Polytope 仓库的位置
按你提供的 2026-10-31 项目结构报告，不要平行复制 Risk/Execution/Paper。推荐：
engine/
  feed/source_runtime/          + Kalshi market data client
  decode/parser/kalshi/         + Kalshi message normalizer
  state/                       + VenueInstrumentKey, quality, synchronized view
  strategy/cross_venue/        + CrossVenueCoordinator / PairScanner / Policy
  risk/                        + multi-venue reservation, residual exposure budget
  execution/adapter/           + Kalshi execution adapter (V0 Shadow only)
  execution/state/             + PairExecutionStateMachine
  paper/ledger/                + two-account receipts, cash, fills
oracle/                         + frozen pair compatibility artifacts
research/cross_venue/          + raw data checks / adverse selection / audits
tools/run_cross_venue_shadow.cpp + dedicated integration composition root
tests/integration/             + snapshots → signals → receipts → PnL
src/scan.py 和 src/pnl_audit.py 是独立、容易运行的参考原型；正式交易核心应该用现有 C++20 体系实现或者封装。不要把这两个离线 Python 文件当作你的生产订单执行器。
共享契约（设计草案，不是现有仓库的编译后类型）
struct VenueInstrumentKey { VenueId venue; std::string market_id; std::string instrument_id; };
struct ApprovedPair {
  PairId pair_id;
  VenueInstrumentKey leg_yes, leg_no;
  Hash settlement_rules_hash_yes, settlement_rules_hash_no;
  uint64_t approval_revision;
  bool enabled;
};
struct BookProvenance {
  uint64_t recv_monotonic_ns;
  uint64_t source_ts_ns;
  uint64_t snapshot_version;
  bool seq_gap, stale, halted;
};
struct CrossVenueOpportunity {
  PairId pair_id;
  uint32_t qty;
  Money expected_total_spend;
  Money worst_case_fee_budget;
  Money slippage_reserve;
  Money estimated_min_profit; // indicative, not realized
  BookProvenance evidence_yes, evidence_no;
};
struct FillReceipt {
  VenueId venue;
  std::string fill_id, order_id;
  PairId pair_id;
  Side canonical_payoff;
  uint64_t qty;
  Money price, fee_paid;
  Timestamp exchange_time, received_time;
};
如果两平台对取消/推迟/平局/客观判定条件处理不同，`ApprovedPair` 必须禁用，不能因为合约题目文字接近就认定完全对冲。
四、处理链路及状态机
A. 市场选择（离线）
1. 拉市场 reference data，只处理已验证、明确可标定的 pair。
2. 在 oracle/ 中配置各平台市场规则版本和所有结算状态 payoff table；人工批准并冻结 artifact hash。
3. 规则不一致、void/cancel 独立结算、截点不同、不同数据源判定 → 禁用，直到能证明等价性。
4. 初期只加载 5–20 个映射，并且优先选择短结算周期和有深度的品种。
B. Feed / Decode / State
1. Kalshi 读取 REST 快照并订阅可用的实时更新（依 API 权限而定）。Polymarket US 可采用 REST L2 和官方 gRPC；若选的是国际版，使用对应的 CLOB 接口。它们是两个不同 venue adapter，不得混用符号、费用或合约规则。
2. 每个原始 packet 保存 venue, instrument, source_ts, recv_monotonic, sequence_or_version, raw_hash, payload。
3. 快照 + delta 必须按交易所规则维护。丢序列、断线、离线时间过长 → state marked invalid, 停止出信号，重读快照。
4. 产生原子可读的两个 venue 视图，比较本机收到的时间戳而不是假定两个交易所的服务端时钟同步。
5. 初始实验过滤：盘口 age <=250ms，receive skew <=100ms。这些是保守的初始假设，不是已经校准的盈利参数。
C. Signal / Order Decision
给定数量 q，计算 L2 深度真实买入成本：
Cost_yes(q) = Σ_i price_yes_ask_i × fill_qty_i
Cost_no(q)  = Σ_j price_no_ask_j  × fill_qty_j
IndicativeFloor(q) = q - Cost_yes(q) - Cost_no(q)
                     - FeeBudget(q) - SlippageReserve(q)
q 从满足最小单位的有限候选集合中选择，最初建议 [25, 50, 100, 200, 500]，但如果盘口不足就不要补足。禁止使用 best ask × q 估算超出第一档的订单。费用要按 venue + product + maker/taker + date/time + 数量版本化；真实账本以交易所回报为准。发现净正价差时，只输出 OpportunityIntent，绝不能将它写到 PnL。
D. Risk
初始 guard：每笔在两平台的总合约名义赔付 <=$500；任意单腿暴露的最坏结算亏损上限 <=$100；不可跨平台共享购买力；每笔限制最小保守利润 >=$1 或者 >=50bps（两者取高）；每次签发 intent 必须锁定 snapshot 版本与 approve revision。这些是供 V0 起步的可修改参数，不是市场事实。
E. Pair Execution State Machine（Shadow）
DETECTED -> RESERVED -> PLANNED
  -> LEG_A_SUBMITTED -> LEG_A_PARTIAL/LEG_A_FILLED
  -> LEG_B_SUBMITTED -> LEG_B_PARTIAL/LEG_B_FILLED
  -> FULLY_HEDGED -> SETTLEMENT_PENDING -> RECONCILED
           \
            -> RESIDUAL_EXPOSURE -> REHEDGE_OR_STOP

Any stage: TIMEOUT / HALT / CANCEL / UNKNOWN / ACCOUNT_DISCONNECT -> FREEZE
两交易所订单无法原子性成交。状态机以 confirmed fill 为唯一库存变化来源；ACK ≠ FILL；CANCEL REQUEST ≠ CANCEL CONFIRMED。订单发送顺序要经过两边盘口深度和延迟实验后决定。不能先验假定“先吃流动性差的一边”必然是最优。

在只读 Shadow 模式：不发订单，也没有真实成交事件，所以真实已实现 PnL 必须是 $0 或未产生仓位；可以独立输出带 SIMULATED 标签的反事实成交结果，但绝不能和 live PnL 合并。
五、PnL 分层与独立核算
指标	数据来源	能否视为实际赚钱
quoted_edge	两平台 L2	否
paper_fill_pnl	Replay / synthetic fill simulator	否
filled_locked_floor	两平台确认成交 + 扣实际费 + payoff matrix	是有条件的合约最差结算值，不是到账收入
realized_pnl	平仓/结算和现金账本	是，前提是 API/账单核对通过
incentive_pnl	奖励已到账账单	是；未到账奖励不得提前计入


对本 V0 的二元 YES 与 NO 长仓：
Payout_if_YES = confirmed_qty_yes × $1
Payout_if_NO  = confirmed_qty_no  × $1
FloorPnL      = min(Payout_if_YES, Payout_if_NO)
                - Σ(qty × price + exchange_fee)
RealizedPnL   = actual_settlement_credit_or_exit_proceeds
                - cash_cost - fees - transfer_costs
适用前提：同一个批准的二元状态空间；两市场同时正常兑现，且一份合约赔付 1 单位一致可比的货币。币种不同必须增加汇率风险。平台违约与规则变更的尾部风险不能通过此式消除。
与交易所对账才算验收
1. exchange_order_id、`exchange_fill_id` 唯一，成交和费用逐条核对到私有 API；重复 fill 不能重复记账。
2. 客户端时间戳、交易所时间戳和原始消息记录都保留；大于容忍时间的回报须标记。
3. starting_cash - actual_spend - fees + received_settlements + confirmed_transfers 必须能对应账户日终余额，所有不匹配都单独入 suspense，不能硬塞入 PnL。
4. 已平仓与已结算的事件才进入 verified_realized_pnl；未结算用 locked_floor、`mark_to_market`、`unhedged_risk` 分开披露。
5. 每次审计记录输入文件 hash、费率版本、规则批准版本、raw market log offset、每腿 fill IDs、截止时间。
六、可以作为 Merge Gate 的验收标准
- Unit: Decimal 精度、不完整盘口、价格跳档、空盘口、数量不足、重复成交、无批准规则、负手续费与反向时间戳全部覆盖。
- Integration: REST snapshot → WS/gRPC stream → State 在断线/增量缺失后正确进入失效状态；不得产生新意图。
- Accounting: 测试 fixture 得出 matched_100 = +1.51, partial_hedge_60 = -18.60, hedge_slippage = -2.49, no_fills = 0.00，差异 <= $0.01。
- Shadow: 连续录制真实行情，机会扫描可以重放获得相同候选 ID、订单簿引用和理论 PnL；成交模拟记录严格与真实账本隔离。
- Live gating: 交易资格、平台用户 API 访问、私人订单回报、结算规则和账户余额对账全部验收后，才能开放审批后的最小规模 real execution。
- Research success: 有足够的真实样本后，统计 edge quoted -> realized 的转化率、机会持续时间、双腿成交率、每腿延迟、残余库存最大亏损、净 PnL 以及容量曲线。不以合成 fixture +1.51 作为策略 alpha 通过的条件。
七、第一批实施事项（按依赖排序）
1. 构造 ApprovedPair artifacts 和 payoff equivalence check（需要人工 rule review）。
2. Kalshi orderbook adapter + 同一 VenueInstrumentKey 类型下的 Polymarket US 数据 adapter；确认选用的具体 Poly 市场版本。
3. 打通 Feed → Decode → State，并维护 WS/gRPC 质量标记、断线后的重建。
4. PairOpportunityScanner 使用 MarketDepthView 和成本/费率模型；保持 OpportunityIntent 版本不变优先。
5. PairExecutionStateMachine 的模拟状态流、成交确认回路、risk ledger 双账户预留。
6. 复用 paper/ledger，扩展双平台币种与费用字段；用 pnl_audit.py 的 fixture 测试进行黄金样例对齐。
7. 新增 tools/run_cross_venue_shadow.cpp，不要在现有 engine/main.cpp 上直接上线。
8. 引入 24–72 小时甚至更长的实测数据，再判断是否值得实现 maker-first v1。
八、参考接口与费用规则
- Kalshi 帮助中心说明费用依合约和订单类型变化，某些产品有特殊费率：https://help.kalshi.com/en/articles/13823805-fees
- Kalshi 费率表：https://kalshi.com/regulatory/fee-schedule
- Polymarket US 只读数据文档给出 BBO、L2 和 gRPC Streaming 接口：https://docs.polymarket.us/data-guide/overview
- Polymarket 国际版是独立 API / 风控 / 费率体系：https://help.polymarket.com/en/articles/13364478-trading-fees
本包不内置一个自称“全球有效”的硬编码手续费率；演示用 fee_budget_usd 和 fee_usd，生产系统必须使用当时、当地、对应产品的费率信息及成交回报。
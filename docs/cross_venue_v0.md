# Cross-Venue V0 实施说明与审计边界

日期：2026-11-01。场所选择：Kalshi + Polymarket 国际版 CLOB。

## 1. 交付范围

本实现交付用户规格中的**离线、合成、可复算 V0**，并将参考流程嵌入现有 C++20 模块体系。没有修改默认 `engine/main.cpp`，没有启用任何真实订单或后台采集进程。

| 验收项 | 当前状态 |
|---|---|
| 冻结配对产物、规则哈希和全表 payoff 互补检查 | 已实现；示例审批是合成标记，不是人工真实规则审批 |
| Kalshi 与国际版 CLOB 消息标准化 | 已实现离线解析和合成协议形状测试 |
| 快照、增量、失效、重建与同步双视图 | 已实现单线程回放路径 |
| L2 成本、显式费用预算、机会意图 | 已实现，复用 `MarketDepthView`、VWAP 和原有 `OpportunityIntent` |
| 独立双账户预留、利润和残余风险限制 | 已实现，复用现有 `RiskLedger` |
| 配对模拟状态机、确认成交、双账户审计 | 已实现，复用现有 `PaperLedger` |
| 四个 PnL 黄金场景、Python/C++ 一致性、重复回放 | 已有自动化验收 |
| 真实行情客户端与持续录制 | 未接通或验收；新增入口只接受 `SYNTHETIC` 回放文件 |
| 真实规则人工 review、FX/抵押品等价性 | 未完成，不能将合成 artifact 改名当作真实批准 |
| 私有订单/成交/结算/余额对账 | 未接入；verified live PnL 始终为空 |
| 连续 24–72 小时 shadow、真实容量与转化率研究 | 尚未开展，不能从 fixtures 得出盈利结论 |
| 真实资金执行 | 无实现开关；适配边界强制拒绝订单提交 |

## 2. 模块分工

| 位置 | 职责 |
|---|---|
| `engine/state/cross_venue/Types.*` | 场所资产键、整数货币、盘口证据、费用预算、机会与成交契约 |
| `oracle/artifact/ApprovedPairArtifact.*` | 规范化产物哈希、审批修订及 payoff 互补检查 |
| `engine/decode/parser/kalshi/KalshiBookNormalizer.*` | Kalshi fixed-point REST/WS 消息转为统一盘口更新 |
| `engine/decode/parser/polymarket/CrossVenueClobNormalizer.*` | 现有国际版 CLOB 消息形状的精确小数边界 |
| `engine/state/cross_venue/SynchronizedBookStore.*` | 独立资产盘口、序列和接收时钟质量、复制双视图 |
| `engine/strategy/cross_venue/PairScanner.*` | 逐档成本、费用/滑点预算、稳定候选 ID、既有意图契约 |
| `engine/risk/ledger/MultiVenueReservationBook.*` | 双账户分别检查并预留资金；全部检查通过才修改账户 |
| `engine/execution/state/PairExecutionStateMachine.*` | 模拟提交/ACK/成交/撤单/冻结/结算待确认状态 |
| `engine/execution/adapter/CrossVenueShadowAdapter.h` | 无凭证、无传输、无 live enable 方法的只读边界 |
| `engine/paper/ledger/TwoAccountPaperLedger.*` | 将确认模拟成交交给两个现有 `PaperLedger`，生成独立审计 |
| `tools/run_cross_venue_shadow.cpp` | 专用离线组装入口与审计文件输出 |
| `research/cross_venue/` | 无依赖 Python 参考计算、合成输入和测试 |
| `tests/integration/cross_venue/` | C++ 契约与链路测试，独立 Python 对照及输出字节比较 |

新增 `engine_cross_venue` 由根 CMake 在现有 Paper 模块之后注册。它是跨模块组装库，未另外复制一套 Risk、Execution、Paper 核心，也未改变公共 `OpportunityIntent` 类型。

## 3. 审批、币种与费用

`ApprovedPair` 固定两个完整的 `VenueInstrumentKey`、两个规则原文哈希、review 标识、approval revision 和 payoff 表。哈希采用带长度前缀的字段编码，避免简单字符串拼接歧义。

每个状态必须满足 `(YES赔付, NO赔付)` 为 `(1,0)` 或 `(0,1)`，同时覆盖两个方向。独立 void、两边不赔、半额兑付等状态在此二元 V0 中直接拒绝。枚举出的表格仍依赖独立人工判断是否覆盖真实全部结算情形，代码不能从标题或哈希证明法律规则等价。

资产键包含币种。两边币种不同会拒绝配对。fixture 使用 **SYNTHETIC_USD**，不假定真实 USD、USDC 或其他抵押品之间可无成本转换。真实接入应根据实际市场元数据核实币种、兑换与尾部风险。

`FeeQuote` 是某个冻结 pair、明确数量、版本和有效时间段下的**分腿总费用预算**与**分腿滑点预算**。订单角色为此 V0 固定的主动买入，未内置跨所有产品有效的费率公式。合成时间段使用回放时钟；真实生产费率必须额外接入场所/产品/订单角色/日期对应版本与实际成交回报。

官方 Kalshi 文档说明不同市场和订单角色可能有不同费用，参见 [费用说明](https://help.kalshi.com/en/articles/13823805-fees) 与 [费率表入口](https://kalshi.com/regulatory/fee-schedule)。国际版费用需按其自身规则核实，参见 [Polymarket Trading Fees](https://help.polymarket.com/en/articles/13364478-trading-fees)。

## 4. 行情与质量边界

### 4.1 标准化

Kalshi parser 支持 `orderbook_fp.yes_dollars/no_dollars` REST 形状，以及 `orderbook_snapshot` 的 `yes_dollars_fp/no_dollars_fp` 和 `orderbook_delta.delta_fp`。YES 的 ask 来自 `1 - NO bid`，反向同理；增量数量为加法。内部先保存精确小数数量，再向下取整为 V0 的整数合约，避免放大可成交量。

REST 不携带 WS subscription 序列，因此 REST 快照不能作为 WS delta 的直接种子。WS 必须从自身快照开始，保持同一个 `sid` 和连续 `seq`。此适配器要求单市场订阅；未来多市场复用同一 subscription 时，需要在订阅层按全流序列分发，不能直接并列使用多个独立解析器跳过其他市场消息。

上述形状和恢复规则来自 [Kalshi Orderbook Responses](https://docs.kalshi.com/getting_started/orderbook_responses) 与 [Orderbook Updates](https://docs.kalshi.com/websockets/orderbook-updates)。

国际版适配器支持仓库已有 CLOB 形状的 `book`/REST snapshot 与 `price_change.price_changes`。数量更新是**价位的绝对量**，不是增量；零量删除。它保留原始小数字符串，避免浮点货币计算。US 的 SDK/gRPC、国际版 unified SDK 的 `topic/payload` 包装不属于此 parser 的输入格式。原始 CLOB 示例见 [官方实时数据文档](https://docs.polymarket.com/market-data/realtime-data)。

国际版这一路的 `local_sequence` 是采集器对已录制包的顺序编号，**不是交易所连续序列保证**。它可以发现本地记录缺包，不能证明交易所或网络从未静默遗漏。生产验收还需权威快照复核、重连重建和足够长的真实数据记录。

### 4.2 状态维护

- 按完整场所资产键隔离状态；跨场所同名资产不会共享盘口。
- Snapshot 替换，Delta 更新；缺少快照、缺序列、断线、HALT、空盘、交叉盘、重复价位或反向接收时钟均阻止信号。
- 已失效的状态不能靠后续 Delta 恢复，必须重读 Snapshot。
- 初始过滤采用 age ≤250ms、receive skew ≤100ms；只比较本机 monotonic receive 时间，外部 source 时间独立保存。
- 现有 `MarketDepthView` 保存最多 64 档；此 V0 对超出边界的输入拒绝，不静默创造或补齐深度。
- 双视图以复制值返回；当前是单线程 coordinator。并行生产 ingestion 必须增加外部串行化或正式原子发布机制。

## 5. 机会与风险

有限数量集合默认为 `[25,50,100,200,500]`。扫描器逐档计算两腿完整买入成本，深度不足直接拒绝：

```text
quoted_edge = q × 1 - cost_yes(q) - cost_no(q)
              - fee_yes(q) - fee_no(q) - slippage_yes(q) - slippage_no(q)
```

正值只代表候选，不写入任何账本。候选 ID 包含规则产物、数量、费用版本与预算、盘口版本/来源证据、累积状态摘要和政策参数。

风控在预留前**重新读取盘口并重算候选**，拒绝证据、规则修订或费用变化。初始限制由 [`cross_venue_v0.shadow.json`](../config/cross_venue_v0.shadow.json) 配置：

- 两腿合计名义赔付 `2 × q × 1 ≤500`。
- 任意单腿的成本、费用与滑点预算总和 ≤100，作为失去另一腿时的保守亏损上限。
- 最小利润为 `max(1, 预算总支出 ×50bps)`；bps 向上取整到货币最小单位。
- 两账户各自拥有足够资金；不能用另一平台余额补足。
- V0 coordinator 同时只允许一个活动配对，候选不能重复预留。

政策是可修改的初始实验条件，不是市场盈利事实。预留只在配对生命周期完成模拟对账、所有订单终态且支出匹配确认回报后释放；冻结或仅发送撤单不释放。

## 6. 执行与记账

配对状态机只解释**显式模拟事件**。A/B 对应规范 YES/NO 腿，允许任一腿先提交，不预设执行顺序最优性。

- ACK 不改变库存，只有确认成交的 delta quantity 才改变库存。
- 撤单请求不等于撤单确认；撤单后迟到的确认成交仍计入。
- 断线、未知或超时等故障由 `freeze(reason)` 进入 FREEZE，禁止新的提交；仍接收迟到成交以免隐藏敞口。
- 未完成两腿覆盖时披露残余风险，示例部分对冲进入 `RESIDUAL_EXPOSURE → REHEDGE_OR_STOP`。
- 超过预留预算的实际模拟成交不被丢弃；记账后冻结，示例 `hedge_slippage` 因此显示亏损和预算超支。
- 满额成交进入 `FULLY_HEDGED → SETTLEMENT_PENDING`，不自行生成到账证明。

两个 `PaperLedger` 以分腿资产键记录现金、份数和实际费用。去重键为 `(venue, fill_id)`；同 ID 同内容重放忽略，同 ID 不同内容报错。对旧账本的数值 ID 转换还检查哈希碰撞。

`TwoAccountPaperLedger` 的余额复核将每个账户的不匹配绝对值累计到 suspense，两边相反方向的差异不会抵消。未提供外部余额时，suspense 的零值仅表示没有输入差异，不代表已经通过账户对账；manifest 明确标记 `NOT_LIVE_VERIFIED`。

## 7. PnL 与输出

对批准的同币种互补二元合约：

```text
cost = 所有确认成交的数量×价格 + 实际手续费
payout_if_yes = qty_yes ×1
payout_if_no  = qty_no ×1
floor = min(payout_if_yes, payout_if_no) - cost
```

这会把未配对部分的最坏损失一起算入，而不是只展示配对部分的收益。四个样例均由模拟 receipt 重算：

| 场景 | YES 份数 | NO 份数 | 总支出（含费） | 条件最差 PnL |
|---|---:|---:|---:|---:|
| matched_100 | 100 | 100 | 98.49 | +1.51 |
| partial_hedge_60 | 100 | 60 | 78.60 | -18.60 |
| hedge_slippage | 100 | 100 | 102.49 | -2.49 |
| signal_no_fills | 0 | 0 | 0.00 | 0.00 |

所有货币计算为 64 位整数 millionths；文本小数最多六位，拒绝超精度、指数、NaN 和溢出。研究参考使用 Python 整数进行相同精度与范围检查，无第三方依赖。

审计保留输入 SHA-256、规则修订和产物哈希、费用版本、每腿 fill ID、原始消息哈希/offset、审计 cutoff 和迟到回报标志。原始 receipt 与其结构化字段还进行一致性检查。哈希是完整性证据，不是来源真实性证明。

`illustrative_settled_pnl_usd` 是人为选择结算方向后的示例复算，`verified_live_realized_pnl_usd` 永远空值。没有私有 API、账单、结算到账和费用对账，本实现不能产生“已验证实盘收益”。

## 8. 复现与后续依赖

完整命令见 [`research/cross_venue/README.md`](../research/cross_venue/README.md)。新增 CTest 包括 C++ 端到端契约检查、Python 单元验证、Python/C++ 黄金结果逐字段一致性和两次回放的全部输出字节比较。

接真实数据前，仍需：选定具体市场并完成人工 rule review；核实实际币种与转换风险；增加场所连接、认证/订阅和原始录制客户端；完成断线重建的真实接口测试；再持续录制 shadow 数据。任何真实成交与收益验收还需要独立的私有回报和现金对账实现。当前没有可以通过配置打开的跨场所真实订单路径。

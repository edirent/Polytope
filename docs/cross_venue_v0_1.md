# Cross-Venue V0.1：真实行情、只读 Shadow 与回放审计

最新接入验证见 [Kalshi authenticated WS / 10 分钟双场所 L2 审计](cross_venue_authenticated_l2.md)；下文原 V0.1 录制结果仍按其历史证据解释。

状态：**PARTIALLY_VERIFIED**。目标是 Kalshi production + Polymarket **国际版 CLOB**；程序没有真实订单提交路径。真实运行的计数、测试结果和结论以 [本次验证报告](cross_venue_v0_1_validation.md) 及其链接的原始审计包为准。

## 1. 运行方式

从仓库根目录执行：

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4

# 仅发现：自动分页、跨类别补充、生成待人工审核队列。
build/cross_venue_discover --config config/cross_venue_v0_1.json

# 10 分钟真实采集，结束后自动生成五种延迟、四种执行模型的报告。
# run-dir 必须是新目录或空目录。
build/run_cross_venue_live_shadow \
  --config config/cross_venue_v0_1.json \
  --duration-sec 600 \
  --run-dir runs/cross_venue/my_new_capture

# 从同一录制离线重放；out 必须是新目录或空目录。
build/run_cross_venue_l2_backtest \
  --run-dir runs/cross_venue/my_new_capture \
  --capital 200000 \
  --latencies-ms 0,100,250,500,1000 \
  --out runs/cross_venue/my_new_capture/replay_again

# 无依赖 Python 独立核算：检查输入/输出哈希、残余亏损、费用和资金上限。
python3 research/cross_venue/audit_v0_1.py \
  --run-dir runs/cross_venue/my_new_capture \
  --analysis-dir runs/cross_venue/my_new_capture/replay_again
```

默认模拟资金上限 200,000，两个 venue 各分配 100,000。可修改配置分配；改变总资金时同步传入 `--capital`，回放不允许超过捕获配置中的上限。全流程不读取真实账户余额，不需要账户资金。

### 可选 Kalshi 行情 WebSocket 授权

缺少授权时，程序继续使用真实公共 REST 快照，并记录 `missing_ws_credentials`。REST 数据只用于明确标记的快照诊断，不能生成连续 L2 执行回测成交。

仅当运行环境已有合法的行情订阅授权时，通过环境提供 `KALSHI_API_KEY_ID` 和 `KALSHI_PRIVATE_KEY_PATH`，或 `KALSHI_PRIVATE_KEY`。签名支持 RSA-PSS/SHA-256 与 Ed25519；签名、私钥、认证头不会写入日志。这里不请求交易密钥，也没有下单开关。

### 72 小时有界采集工作流

```bash
python3 research/cross_venue/run_bounded_campaign.py \
  --config config/cross_venue_v0_1.json \
  --hours 72 --segment-minutes 30 --max-total-gb 8 \
  --out runs/cross_venue/my_new_72h_campaign
```

该命令由 Python 调度**原生 C++**，每段重新发现、初始化订阅和核实费率来源，保留显式断线/重建边界。它是有期限和总存储预算的采集工作流；未在开发时自动启动。收到中断或达到预算时停止，并保存状态。

每段报告是独立 Shadow 实验。不能直接相加各段的 PnL/容量，不能将它们说成没有断点的连续 L2，也不能假设跨段的库存已经结算。跨段统一资金回放与已确认结算后的资金释放仍是后续工作。单段回放目前将录制读入内存，因此长采集采用有界分段，而非承诺任意大小数据的一次性回放。

## 2. 数据证据与输出

真实录制目录保存：

- `market_metadata.json`、`market_discovery.csv`：完整保存采样到的市场原始元数据、规则及其哈希、协议、币种、状态、盘口和费用字段。
- `metadata_requests.jsonl`、`metadata_response.*.json`：每次元数据请求的 URL、HTTP 状态、错误、响应原文、接收时间与哈希。
- `candidate_pairs.json/csv`：标题词、参与者/日期线索、类别、匹配评分与风险旗标；全部只是审核建议。
- `approved_pairs.json`、`approval_rejections.json`：本次实际采用的冻结规则产物与拒绝理由。
- `selected_markets.json`、`fee_source_audit.json`、`fee_response.*.json`、`fee_models.frozen.json`：实际观察对象和版本化费率依据。
- `kalshi.raw.N.jsonl`、`polymarket.raw.N.jsonl`：逐包 append-before-process 的原始数据。记录 venue、资产身份、来源、UTC/monotonic 时间、session epoch、arrival sequence、sid/seq/clock、源时间、完整 payload 和哈希。
- `connection_events.jsonl`、`capture_health.json`、`market_health.csv`：连接、授权缺失、断线、重建、盘口质量与健康状态。
- `capture_manifest.json`、`source_manifest.json`、`source_snapshot/`：时间窗口、配置哈希、分页完整性、数据计数、原始文件清单及代码来源。

分析目录保存规格要求的 `opportunities.csv`、`shadow_trades.csv`、`pnl_by_trade.csv`、`pnl_by_market.csv`、`latency_sensitivity.csv`、`capital_utilization.csv`、`summary.json`、`report.md`、`audit_manifest.json`，并复制发现/健康 CSV。

捕获代码和分析代码可能在修复 parser 时属于不同工作区版本，因此分析还保存独立的 `analysis_source_snapshot/` 与 `analysis_source_manifest.json`。Git revision 不能单独代表这些未提交实现；应同时检查代码文件哈希。

任何 HTTP 403、429、认证或网络错误都原样记录；不改用代理、替代地域或绕过访问控制。一次连接成功、一个实现类存在、或者一份合成 fixture 都不算真实 L2 或收益验收。

## 3. 规则、协议与币种

`config/cross_venue_approved_pairs.json` 默认空。自动匹配不会批准任何 pair。实际加载必须通过已有 `ApprovedPairArtifact` 的 enabled/review/revision/hash、完整二元 payoff 互补检查，且绑定当前元数据规则哈希。

真正可用的审批还需要 `economic_review.status=APPROVED` 与 reviewer，确认实际资产、结算条件和货币。当前 V0 合同仍拒绝不同币种；没有把 USD、USDC、未知抵押品归为同一种无风险资金。未审查的 FX/depeg/转换假设不会进入所谓“保证套利”。本版尚未提供通用跨币种转换模型。

支持的国际版分析子集是确认的 `v1` CTF 二元标识。`v2`/未知协议不按 CTF token 解析；Polymarket US 也不进入国际版适配器。NegRisk 等额外结构进入待审核状态，不自动沿用简单二元结算假设。

由于一些公共元数据不直接提供数量步进，未知字段会阻止资格通过。必要时可以在配置的 `reviewed_instrument_constraints` 数组里提供 venue、market_id、minimum_quantity、quantity_step、tick、source、source_sha256 和 `review_status=APPROVED`；这些是明确、来源固定的审核补充，不是默认猜测。

## 4. 盘口与时间

- 复用 `SynchronizedBookStore`，并保留默认 V0 行为。原生 V0.1 精确保留 micro-share 数量，货币运算使用整数和 int128 中间值。
- 运行视图最多保留 64 档**已验证的最佳深度前缀**；完整快照和消息仍保存在 raw。不能把这个视图称作完整无限深度，不能补齐隐藏数量。超出已知前缀的容量不被假定存在；已知深度耗尽时停止/重建。
- Kalshi signed size delta 与 Polymarket absolute size update 分开处理。Kalshi 在 subscription 层检查跨市场连续 `seq`，再交给各市场 parser；REST 不能初始化 WS 的 subscription sequence。
- 分别记录最后市场更新、权威快照、连接心跳、接收时刻和版本。安静市场不因“最近没有成交/价格变动”就必然被判陈旧；连接和权威状态必须分别满足限额。
- 默认 receive skew 100ms、连接超时 30 秒、权威状态最长 45 秒；REST 降级轮询 5 秒，已建立流的权威比较间隔 15 秒。均是实验参数。
- WS 断开、缺序列、格式异常、tick/status 变化或权威比较失败会禁用相关信号，并重建；本地 arrival sequence 绝不是交易所不丢包保证。

## 5. 扫描、费用与模拟执行

复用原有 `PairScanner`、`OpportunityIntent`、VWAP、`MultiVenueReservationBook`、`PairExecutionStateMachine` 和 `TwoAccountPaperLedger`。没有另建交易引擎或 PnL 账本，也没有修改默认 `engine/main.cpp`。

扫描只针对批准的互补市场，并从同一审批的二元逻辑透明推导反向组合。有限数量集合 `[25,50,100,200,500]` 需满足实际最小量、步进、可见深度和风险政策。排名先看净金额，再看资本收益率，重复的同一流动性不当作独立成交。

费用依据当前所选市场保存的元数据：国际版使用 Gamma 的显式 fee schedule；Kalshi 查询事件与系列的实际 fee type/multiplier，并引用冻结的、带日期的一般费率模型。未知/非支持类型拒绝；不会给所有产品套一个费率或默认免费。费用估计有源 URL、版本和观察时间。Kalshi 采用保守的非直接会员逐片段向上取整资金预算，不假定 rebate，估计不等于真实收费回报。

费率事实来源：[Kalshi 当前公开费率表](https://kalshi.com/docs/kalshi-fee-schedule.pdf)、[账户与费用舍入](https://docs.kalshi.com/getting_started/fee_rounding)、[Series API](https://docs.kalshi.com/api-reference/market/get-series)、[Polymarket Fees](https://docs.polymarket.com/trading/fees)。原生网络输入和冻结模型文件使本次估计可复算；后续长时间运行还需持续复核模型与场所政策是否更新。

观察到的逐档成本、未来滑点预算与执行风险预算分别披露，禁止重复收费。接收新证据后重新验证盘口、规则、费用和资金；ACK 不产生仓位，撤单请求不释放预留。

回放执行场景是可见深度的假设上界、第二腿 60%、第二腿失败、第二腿额外恶化 4¢。后面三项是明确的压力假设，不能说成观察到的成交。每项分别跑 0/100/250/500/1000ms，并在到达时读取之后已收到的状态；同时间后到的消息也不能提前使用。

资金由共享的既有 RiskLedger 分 venue 预留；不把同一资金同时借给多个机会。模拟消费的价位流动性扣减后不会凭空补充。未观察到可信结算时，仓位和资金保守保持占用；锁定时长为到录制结束的下界。该策略会低估可循环使用的容量，但不会制造已经回笼的资金。

`verified_live_realized_pnl` 始终为 **null**。Quoted edge、延迟后优势、假设合约 floor、未对冲最坏损失、MTM 和现金已实现收益分开。纯 REST 的观测只输出诊断，不能伪装成连续 L2 执行回测。

## 6. 修改模块索引

| 文件或模块 | 本次变化 |
|---|---|
| `engine/feed/source_runtime/CrossVenuePublicData.cpp` | 原生 HTTPS GET、签名、时间/配置、有限存储和 raw rotation |
| `engine/feed/source_runtime/CrossVenueLiveCapture.cpp` | 自动发现后的多 venue 采集、重连、心跳、快照比较、现场审计 |
| `engine/feed/source_runtime/WebSocketClient.h/.cpp` | 在现有客户端增加签名握手头和控制帧回调 |
| `engine/strategy/cross_venue/LiveResearch.h` | 原生发现、采集、回放与研究结果的公共接口 |
| `engine/strategy/cross_venue/MarketDiscovery.cpp` | 分页、跨类别候选、审核门、元数据和逐市场费率来源 |
| `engine/strategy/cross_venue/ResearchState.cpp` | 统一复用实时/回放处理，源时钟、质量、状态与事件驱动扫描 |
| `engine/state/cross_venue/Types.h`、`SynchronizedBookStore.h/.cpp` | 健康证据、精确份数、前缀、订阅序列及权威状态比较 |
| `engine/common/math/FractionalVwap.h` | 精确分数深度的整数货币成本计算 |
| `engine/decode/parser/kalshi/KalshiBookNormalizer.h/.cpp` | subscription 路由、clock、分数数量支持 |
| `engine/decode/parser/polymarket/CrossVenueClobNormalizer.h/.cpp` | 精确数量、legacy/unified 边界和深度前缀 |
| `engine/strategy/cross_venue/PairScanner.cpp` | 新精度与健康政策、完整证据绑定，保持旧契约 |
| `engine/risk/ledger/MultiVenueReservationBook.h/.cpp` | 多配对共享两个真实分开的模拟钱包，确认支出和保留风险 |
| `engine/execution/state/PairExecutionStateMachine.cpp`、`engine/paper/ledger/TwoAccountPaperLedger.cpp`、`engine/state/cross_venue/Types.cpp` | 允许真实市场输入产生明确 SIMULATED 回报，仍禁止 live receipt |
| `engine/strategy/cross_venue/FixtureIO.h/.cpp` | 保持合成默认；显式分离真实审批的加载 |
| `replay/cross_venue/LiveL2Replay.cpp` | 确定性五延迟/四场景、资金约束、残余损失和自动报告 |
| `tools/cross_venue_research_main.cpp` | 三个独立 CMake 运行入口 |
| `engine/strategy/cross_venue/CMakeLists.txt` | 原生模块、工具及新增测试注册 |
| `config/cross_venue_v0_1.json`、`cross_venue_approved_pairs.json`、`cross_venue_fee_models.json` | 实验、空审批门与有日期的费用模型配置 |
| `tests/integration/cross_venue/cross_venue_v0_1_test.cpp`、`independent_v0_1_audit.py` | 真实协议样本、故障、延迟、资金、签名、PnL 与独立核算 |
| `tests/fixtures/cross_venue/real_protocol_samples.json` | 实际捕获的公共协议样本及 provenance；仅回归测试 |
| `research/cross_venue/audit_v0_1.py`、`run_bounded_campaign.py` | 独立只读审计和有界原生采集调度 |

## 7. 未验收部分

缺少 Kalshi 行情 WS 授权的能力仍然是不完整的，REST 成功不能代替它。所有真实规则、数量约束和币种审批仍需独立完成。当前没有通用 FX/depeg 模型、跨段统一组合回放、真实订单接口或私有回报对账。不能把本次无合格交易解释成“市场上不存在套利”，也不能把任何合成正例解释成 alpha 已经通过验证。

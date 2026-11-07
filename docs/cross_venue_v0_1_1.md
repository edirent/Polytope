# Polytope V0.1.1：候选规则审核、行情故障与真实证据

后续更新：[Kalshi signed WS 与完整 10 分钟双场所采集已验证](cross_venue_authenticated_l2.md)。本文保留前一轮录制和缺少授权时的历史结果；认证接入成功没有批准任何经济配对。

验收状态：**PARTIALLY_VERIFIED**；经济结论：**NO_APPROVED_PAIRS**；用户要求的“获批配对真实同步 L2 与有经济意义回测”**尚未达成**。本轮实际完成全部 200 个候选的规则分级、20 份详细档案、官方规则/费用/盘口补采、15 次旧断线分析、原生修复、新的 10 分钟采集、20 场景回放和独立审计。

没有真实订单；`verified_live_realized_pnl = null`。24/72 小时研究采集未启动，24 小时预检的阻断结果已保存。

## 1. 交付入口

- [完整证据报告与 20 场景表](../runs/cross_venue/v0_1_1_evidence_20261105/report.md)
- [全部 200 个候选排序](../runs/cross_venue/v0_1_1_review_final_20261105/ranked_candidates.csv)
- [前 20 个规则档案索引](../runs/cross_venue/v0_1_1_review_final_20261105/report.md)
- [完整合约 PDF、提取文本与逐项补充判断](../runs/cross_venue/v0_1_1_review_final_20261105/supplementary_terms/report.md)
- [待人工审核 manifest](../runs/cross_venue/v0_1_1_review_final_20261105/approval_manifest.pending.json)
- [旧录制原生故障审计](../runs/cross_venue/v0_1_1_original_feed_audit_verified/summary.json)
- [新录制原生故障审计](../runs/cross_venue/v0_1_1_live_feed_audit/summary.json)
- [新录制的回放 CSV](../runs/cross_venue/v0_1_1_live_20261105/analysis_v0_1_1/latency_sensitivity.csv)
- [Kalshi 实际握手诊断](../runs/cross_venue/v0_1_1_kalshi_ws_probe.json)
- [24 小时预检阻断证据](../runs/cross_venue/v0_1_1_24h_preflight/preflight.json)

原 V0.1 的原始录制与分析目录没有覆盖。新报告区分旧候选队列与新采集窗口，不合并两次运行的 PnL 或观察数。

## 2. 全部候选与前 20 个规则档案

全部 **200** 个候选逐项比较具体事件类别、选项公司/人物、期限、价格来源、平局条件、元数据流动性、费用可得性、协议可用性和持有期代理。排名是确定性的研究优先级，不能证明规则等价。持有期代理来自合同期限；不把交易关闭时刻当作现金到账时刻。

结果：**9 个语义较接近、仍需人工审核；191 个有明确矛盾或属于不同事件**。200 个全部未经人工批准。

| 排名 | 候选类别 | 主要判断 |
|---:|---|---|
| 1 | 2027 年前美国确认外星生命 | 时间边界、Cabinet 列举、官方发布/私人身份及可信报道证据的范围仍需核实 |
| 2–9 | 2028 年民主党总统提名，8 位具体人物 | 两边均要求获得并接受提名；Kalshi 完整条款采用首位获提名并接受的人，Poly 明确后续替换不改变结算；还需全状态、NegRisk、作废/争议和资产审核 |
| 10–16 | 11 月末 AI 排名 | Kalshi 的不同公司被旧标题匹配配给了 Poly Z.ai；完整合约另有 10:00 ET 对 12:00 ET、不同平局与 AutoEval 政策，不能批准为同一事件 |
| 17–19 | 外星生命，不同截止日期 | 存在明确期限差异 |
| 20 | 当日 BTC 价格 | CF BRTI 与 Binance BTC/USDT 来源不同，另有不同阈值和观察时间 |

原始队列中较短期的 BTC 和体育候选没有提供可证明的同一 payoff。优先短期不意味着让同名电竞单场对冲赛季冠军，或让不同价格指数成为同一结算变量。

档案包含两边完整 API 规则、规则哈希、实际官方来源、取消/作废/延期/平局条款或明确缺失标记、币种、数量和 tick 字段、市场费用、盘口可达性、五类 payoff 状态及审批阻断。顶级候选涉及的 **70 次**官方 API 请求全部 HTTP 200。另取得 **8 份**完整合约/认证 PDF，均 HTTP 200，保存原文、页级文本和哈希；Poly resolution 文档也已保存。Kalshi 通用 Rulebook 页面实际返回 **429**，没有把该部分写成已核实。

所有档案的 `coverage_proven=false`。普通 YES/NO 两行不能排除跨场所判断不一致或作废分支；不删除这些状态以制造完整覆盖。

## 3. 明确人工修订的摄入

V0.1.1 配置启用 `require_explicit_revision_review=true`，仍使用原 `ApprovedPairArtifact` 和原审批文件，不另建规则引擎。

摄入的 artifact 必须通过 V0.1 原有规则哈希、交易状态、协议、币种、数量和 payoff 校验；还须匹配 `human_review_manifest` 中的确切记录：

- `candidate_pair_id` 等于 `pair_id`；
- `status` 和 `decision` 均为 `APPROVED`；
- reviewer 与 reviewed_at 不为空；
- `approved_revision` 和 `approved_artifact_hash` 绑定该版本；
- 有合法的 `dossier_sha256`，并明确确认 coverage、settlement assets、quantity constraints。

当前 manifest 所有记录仍为 `PENDING_HUMAN_REVIEW`，`pairs=[]`。没有替用户填写 reviewer、批准决定或经济假设。原 V0 币种证明仍拒绝 USD 与其他资产直接等同；真实抵押品/转换、depeg 风险不能仅通过写一个审批标记消失。

## 4. Kalshi WS 诊断

现有环境的 `KALSHI_API_KEY_ID`、`KALSHI_PRIVATE_KEY_PATH`、`KALSHI_PRIVATE_KEY` 均为空。一次不带认证的官方 production WS 握手实际返回 **HTTP 401**；这证明路径可达及需要认证，不证明签名订阅成功。

原生签名实现保留，RSA-PSS/Ed25519 的离线校验通过。新增对 Kalshi `error` 订阅消息的显式失效与诊断，避免连接建立但订阅拒绝被当成健康无更新。重连后重签名、按 sid 跟踪 seq、REST 不初始化 WS 序列的边界保持不变。

需要现成的合法行情授权配置；无需资金，也不要求在聊天中发送密钥。认证握手及初始 snapshot→delta 的真实验收仍缺失，REST 始终只是诊断。

官方依据：[Kalshi WS 认证与签名](https://docs.kalshi.com/getting_started/quick_start_websockets)、[Polymarket 实时数据](https://docs.polymarket.com/market-data/realtime-data)、[Polymarket 结算与争议流程](https://docs.polymarket.com/concepts/resolution)。

## 5. 旧 15 次断线与本轮修复

原始 5569 条记录经文件哈希、payload 哈希、偏移及本地 arrival sequence 校验后，由原 `ResearchState` 重放。

- 15 次重建均紧随 REST 比较失败，符合 V0.1 客户端主动断开路径；没有服务端主动关闭的证据。
- 13 次首个差异集中在第 61–64 档，符合旧深度前缀裁剪丢失实际已见深层价位的问题；2 次 REST 源时间早于 WS 状态。后者不能证明 WS 漏包。
- 断开到重新建立累计 **23.366 秒**；全部所选 Poly 资产的健康 WS 覆盖 **572.811 秒**。
- 本地记录序列缺口为 0；CLOB 没有连续的公共交换序列，因此交易所实际漏失消息数为 **unknown/null**。

修复在原 `SynchronizedBookStore` 内保留最多 10,000 个实际观察到的价位，供更新与价位提升使用；执行读取仍最多最佳 64 档。没有新增盘口引擎，没有补造隐藏深度。默认 V0/V0.1 行为保持，V0.1.1 通过配置启用。

REST 请求现在记录开始 monotonic 时间。发生不一致且请求期间有 WS 更新，或 REST 源时间更旧时，标记 `RECONCILIATION_INCONCLUSIVE`；不覆盖 WS，不延长权威有效期。真正静默请求窗口内的不一致继续失效和重建。拒绝现在保留锁价、空侧、序列和价位上限等具体原因。

## 6. 新真实采集与回放

UTC **2026-11-05 16:26:27–16:36:27**，芝加哥 **10:26:27–10:36:27 CST**，实际 **600.110 秒**。观察的是原录制同一批 8 个市场，以核验采集路径；不将它们标为已批准的配对。

| 指标 | 本轮结果 |
|---|---:|
| 分页采样发现市场 | Kalshi 797，Poly 1199 |
| Raw 记录/字节 | 11,348 / 23,648,866 |
| Kalshi 成功 REST 盘口 | 349 |
| Poly 成功 REST 盘口 / WS 行情帧 | 290 / 10,638 |
| Poly 有效资产盘口观察 | 15,011 |
| 致命采集错误 | 0 |
| Poly 重建 | 4 |
| 断开到重开停机 | 7.087 秒 |
| Poly 至少一个资产健康 WS 覆盖 | 591.211 秒 |
| Poly 全部所选资产健康 WS 覆盖 | 588.664 秒 |
| 获批连续跨场所同步覆盖 | 0 秒 |

4 次重建：2 次由真实重构后的锁价盘口触发，另 2 次由静默请求窗口内的 REST/WS 不一致触发；后者不强行归因为交易所丢包。另有 11 次比较无法判定。新旧时段市场活动不同，不能把两组计数当作受控实验或稳定性达标证明。

五延迟与四执行模型均已运行。总资金上限 200,000、两独立钱包、逐市场费用、部分/失败/恶化压力、重复流动性扣减及未观察结算不释放资金等原约束保持。

所有 20 场景：合格机会 0，模拟交易 0，资金占用 0；存活率、gross/net PnL、费用和损失表现、容量、机会持续时间、残余风险均**未测/null**。空审批不代表市场无套利。

转换漏斗使用原受审核的同一候选样本：**2002 个市场 → 200 个候选 → 20 份详细审核档案（200 个全量分级，人工决定 0）→ 0 获批 → 0 健康获批同步观察 → 0 gross positive → 0 net positive → 0 延迟存活 → 0 模拟交易**。

两次独立回放的 **40 个文件逐字节一致**；独立 Python 审计通过 **119 个哈希**。这是本地一致性与算术验收，不是交易所成交认证。

## 7. 构建、测试与模块变化

执行 `cmake -S . -B build`、`cmake --build build -j4`、全量 CTest 和 Cross-Venue 子集。构建成功；Cross-Venue **6/6** 组通过，其中 V0.1/V0.1.1 原生 **138 项检查**及新增 **5 项 Python 规则/采集门检查**通过。V0 的 97 项 C++、15 项 Python 和跨语言黄金回归继续通过。

全仓 **1028 项注册：1022 通过，1 失败，5 禁用**。失败仍是原有 `AvalancheSkewsQuotesAndRiskCutsBuyChannel`；没有修改做市代码或弱化测试。[完整 CTest 证据](../runs/cross_venue/v0_1_1_evidence_20261105/validation/ctest.xml)。

| 模块 | 改动目的 |
|---|---|
| `SynchronizedBookStore.h/.cpp` | 在既有 store 内保留实际深度；输出仍 64 档；具体拒绝原因 |
| `LiveResearch.h`、`ResearchState.cpp` | 异步比较诊断、原始来源时间、订阅错误、WS 来源健康证据 |
| `CrossVenueLiveCapture.cpp` | 请求开始时钟、诊断文件、明确重建原因、有界重连及长任务前提 |
| `MarketDiscovery.cpp` | 精确人工修订审批门；研究目标定向补采，避免分页漏掉已审核市场 |
| `LiveL2Replay.cpp` | 同步覆盖只计 WS+WS，独立于是否正价差，并截断健康过期时间 |
| `FeedAudit.cpp` | 复用原状态与已校验 raw reader，统计逐资产覆盖、断线与比较差异 |
| `cross_venue_research_main.cpp`、原模块 CMake | 新 `cross_venue_feed_audit` 入口及测试注册 |
| `review_v0_1_1.py`、`extract_review_terms.py` | 确定性规则分级、完整档案、来源留存和 PDF 提取 |
| `diagnose_kalshi_ws.py`、`report_v0_1_1.py` | 只读握手诊断、转换漏斗和证据报告 |
| `run_bounded_campaign.py` | 24/72 小时预检：缺授权或明确获批修订时不启动 |
| `config/cross_venue_v0_1_1.json`、Cross-Venue 测试/真实样本 | 可复现配置、人工审批、深度提升、异步比较和真实锁价回归 |

## 8. 复现与后续 24/72 小时工作流

```bash
ctest --test-dir build -R '^CrossVenueV0' --output-on-failure

python3 research/cross_venue/review_v0_1_1.py \
  --run-dir runs/cross_venue/v0_1_validated_20261105 \
  --out runs/cross_venue/new_empty_review --refresh

build/cross_venue_feed_audit \
  --run-dir runs/cross_venue/v0_1_1_live_20261105 \
  --out runs/cross_venue/new_empty_feed_audit

build/run_cross_venue_l2_backtest \
  --run-dir runs/cross_venue/v0_1_1_live_20261105 \
  --capital 200000 --latencies-ms 0,100,250,500,1000 \
  --out runs/cross_venue/new_empty_analysis

python3 research/cross_venue/run_bounded_campaign.py \
  --config config/cross_venue_v0_1_1.json --hours 24 \
  --segment-minutes 30 --max-total-gb 8 --require-research-ready \
  --out runs/cross_venue/new_24h_campaign
```

最后一个命令在当前前提缺失时以退出码 2 保存阻断原因；不会启动空审批研究任务。先独立解决规则/资产/数量约束与签名行情授权，再在健康路径中观察 24 小时；稳定后把 `--hours` 改为 72，另用新目录。工作流是有界分段，段间重建明确保留，各段独立组合；不能相加为连续统一资金回测。跨段持仓结算仍需后续统一账务支持。

PDF 文本从保存的原文提取可使用 `extract_review_terms.py --directory <supplementary_terms>`；本轮可选研究依赖为 `pypdf==6.21.4`，安装在临时目录，没有增加生产引擎依赖。

到 V0.2 的最小门槛：完成并明确批准具体规则修订、真实资产和数量精度证明，提供有界 FX/depeg 假设或同币种合约证明，验证 Kalshi signed snapshot/delta；随后取得有足够健康同步覆盖的数据。Maker-first 另需队列和撤单延迟证据。真实执行仍需要独立授权及私有订单、成交、结算对账，本版不启用真实下单。

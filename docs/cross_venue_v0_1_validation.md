# Cross-Venue V0.1 实际验证报告

验收状态：**PARTIALLY_VERIFIED**。经济结论：**NO_APPROVED_PAIRS**。

真实数据已录制并离线重放。当前证据不能证明存在经济上可执行的跨场所套利，也不能证明市场上不存在此类机会。没有真实订单或成交，`verified_live_realized_pnl = null`。

## A. 代码与运行边界

新增原生 C++ 市场发现、HTTPS/签名 WS、分 venue 原始录制、健康与权威快照比较、事件驱动扫描、五延迟/四模型回放和自动报告。复用 V0 的 Oracle、盘口、意图、风控、状态机和双账户 PaperLedger；默认 `engine/main.cpp` 保持原样。

[完整文件与模块清单](cross_venue_v0_1.md#6-修改模块索引)列出确切路径、改动理由及运行方法。原有未提交的做市等工作未被覆盖。

## B. 构建与测试

实际执行：

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4 --output-junit /tmp/polytope-v01-ctest.xml
ctest --test-dir build -R '^CrossVenueV0' --output-on-failure
```

- 全量构建成功。
- 全仓共 1027 项注册测试：**1021 通过、1 失败、5 项按原配置禁用**。
- Cross-Venue 相关 **5/5 CTest 通过**，含原 V0 的 97 项 C++ 检查、15 项 Python 参考测试与黄金结果一致性；新增 V0.1 有 122 项原生检查及独立 Python 审计。
- 新测试覆盖真实协议样本、RSA/Ed25519、精确分数深度、缺序列/断线、tick/protocol/币种门、延迟重定价、部分与失败对冲、资金占用、原始文件变更和禁止 live PnL 伪装。多持仓测试验证残余风险按未平仓组合累计，资金收益率分母包括实际承诺资金。

**未解决的全仓失败**：`AvalancheSkewsQuotesAndRiskCutsBuyChannel`，断言 `reducing ask below fair`。隔离原始 target 的链接命令后，确认它没有链接 Cross-Venue 对象/库，仍复现失败：`ask=509999, fair=500000, skew=74985`。现有做市的 post-only 报价限制与旧断言冲突。相关 QuoteEngine、MarketMakingEngine、QuoteRiskEvaluator 文件与本轮早期源码快照逐字节一致；本次没有修改该策略或放宽测试。

证据：[CTest XML](../runs/cross_venue/v0_1_validated_20261105/validation/ctest.xml)、[完整日志](../runs/cross_venue/v0_1_validated_20261105/validation/ctest.log)、[隔离复现](../runs/cross_venue/v0_1_validated_20261105/validation/unrelated_mm_isolation.log)。这项失败仍需在原做市工作中解决，不能将全仓验收写成全部通过。

## C. 真实采集

- 芝加哥时间：**2026-11-05 00:50:04 CST — 2026-11-05 01:00:04 CST**。
- UTC：`2026-11-05T06:50:04.190602486Z` — `2026-11-05T07:00:04.311981448Z`。
- 实际采集 **600.121 秒**，原始数据 **13,756,282 字节**；致命采集错误 **0**。
- 发现 **806 个 Kalshi 市场、1196 个 Polymarket 市场**。这是配置限额内的分页采样，不是完整市场普查；分页结果已按 ID 去重。
- 保存 **200 个**待审核建议（队列上限 200），**0 个**人工批准配对。
- 实际观察 8 个市场，覆盖经济、选举、体育等类别。

| 场所 | 成功 REST 盘口响应 | WS 行情消息帧 | 有效资产盘口观察 | 实际路径 |
|---|---:|---:|---:|---|
| Kalshi | 420 | 0 | 840 | 真实 REST 快照采样；缺少授权，WS 未验收 |
| Polymarket 国际版 | 312 | 4747 | 4861 | 真实 CTF-compatible WS + REST 比较 |

共保存 **5569 条原始记录**（包括行情、控制和连接事件）。Kalshi 一个响应提供 YES/NO 两份盘口，WS 一帧也可能包含多个资产，因此“有效盘口观察”不等于独立网络消息数。

Polymarket 有 16 次连接建立、15 次显式重建；15 次盘口/权威比较拒绝触发保守恢复。重连空档没有被填补成连续数据。按已批准 pair 计算的有效同步覆盖是 0.0 秒，原因是审批为空，不能据此推断网络没有数据。

初次预检中某些 CLOB 请求出现 HTTP 403；本次最终选择的市场返回成功响应。没有使用替代地域、代理或访问绕过。Kalshi WS 未提供行情凭证，相关能力明确保留为未完成。

首轮额外的 10 分钟 pilot 暴露 Gamma 大 limit 实际只返回 100 条和深度超过 64 档的问题；修复后完成了本次独立 10 分钟复跑。原始完整深度全部留存，运行视图明确限为最佳 64 档已知前缀。

例如最高分候选为两边标题近似的「Will the U.S. confirm that aliens exist before 2027?」。标题相近仍未被当作规则等价或风险消失的证据。

## D. 实际录制的回放结果

已在真实录制上运行 **0/100/250/500/1000ms × 4 个执行模型**。下表列出可见深度基准；另外的 60% 第二腿、第二腿失败、第二腿恶化 4¢ 模型同样没有可审批交易。

| 延迟 ms | 合格机会 | 存活率 | 资格资金 | 模拟交易 | 模拟 gross PnL | 费用/执行损失 | 模拟 net PnL | 最大残余风险 | 最大资金使用率 |
|---:|---:|---|---:|---:|---|---|---|---|---:|
| 0 | 0 | 不可计算 | 0 | 0 | 未测 | 未测 | 未测 | 未测 | 0% |
| 100 | 0 | 不可计算 | 0 | 0 | 未测 | 未测 | 未测 | 未测 | 0% |
| 250 | 0 | 不可计算 | 0 | 0 | 未测 | 未测 | 未测 | 未测 | 0% |
| 500 | 0 | 不可计算 | 0 | 0 | 未测 | 未测 | 未测 | 未测 | 0% |
| 1000 | 0 | 不可计算 | 0 | 0 | 未测 | 未测 | 未测 | 未测 | 0% |

“未测”对应 JSON 的 **null**，不是零利润、零费用或证明没有套利。没有批准的 pair，也没有跨两边的连续 WS 证据，因此不能伪造一张盈利回测表。部分对冲与负 PnL 的实现正确性由明确标记的合成回归覆盖，不作为实证交易结果。

资金上限为 200,000，初始独立钱包各 100,000。本次实际模拟占用为零，且无持仓或结算回款可用于计算资金循环效率。未到账、未观察的结算不释放资金。

## E. 复现与经济结论

```bash
build/run_cross_venue_l2_backtest \
  --run-dir runs/cross_venue/v0_1_validated_20261105 \
  --capital 200000 --latencies-ms 0,100,250,500,1000 \
  --out runs/cross_venue/v0_1_validated_20261105/another_empty_analysis

python3 research/cross_venue/audit_v0_1.py \
  --run-dir runs/cross_venue/v0_1_validated_20261105 \
  --analysis-dir runs/cross_venue/v0_1_validated_20261105/analysis_final_reproduced
```

实际独立复跑比较了 **39 个文件，差异 0**。Python 独立核验了 **109 个哈希**，包括原始数据、元数据/费率响应、分析输出及分析源码；结果为 `LOCAL_AUDIT_PASSED`。这只证明本地一致性与算术，不认证交易所成交或收益。

[自动研究报告](../runs/cross_venue/v0_1_validated_20261105/analysis_final_reproduced/report.md)、[全部延迟/执行模型 CSV](../runs/cross_venue/v0_1_validated_20261105/analysis_final_reproduced/latency_sensitivity.csv)、[结果 JSON](../runs/cross_venue/v0_1_validated_20261105/analysis_final_reproduced/summary.json)、[审计清单](../runs/cross_venue/v0_1_validated_20261105/analysis_final_reproduced/audit_manifest.json)、[重复回放证明](../runs/cross_venue/v0_1_validated_20261105/validation/determinism.json)。

**经济判断：证据不足。** 当前不能确认具有经济意义、可实际执行的跨场所套利。最关键的未满足证据是：

1. 人工批准的规则/经济配对为 0。
2. 所选市场有 8 个缺少已验证数量步进，4 个缺少已确认抵押资产。
3. Kalshi 缺少 authenticated WS 行情授权，当前仅为 REST 诊断路径。
4. 全仓另有 1 项原做市测试未通过。

所选市场的费用模型已取得来源与版本（未知费率计数 0）；这不是已付款账单核对。通用 USD/USDC/其他抵押品转换、depeg 和跨段统一结算资金回放尚未提供，保持拒绝而不是假定等价。

## F. 最小后续工作

先从待审核队列选择少量市场，独立审查完整规则、币种/抵押品、数量步进及费率来源；补齐必要的转换与尾部风险模型。随后在有合法行情授权的环境完成 Kalshi WS 和更长双场所健康录制，再测报价优势到保守成交的转换率与容量。还需解决已隔离的原做市测试冲突。

Maker-first V0.2 需要另加并验证队列位置与撤单延迟模型。任何真实小额执行都需要独立授权、订单/成交私有回报和结算现金对账；本版没有相应真实订单开关。本次没有启动 72 小时任务，其有界原生采集调度命令已在[使用说明](cross_venue_v0_1.md#72-小时有界采集工作流)中给出。

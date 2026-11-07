# Kalshi authenticated WS 与双场所 L2 实际验证

结论：**Kalshi 认证接入阻塞已解除**。已用用户 WSL 终端中的现有环境完成 Ed25519 signed production WS，收到真实 snapshot 和后续 delta，执行了完整 600 秒双场所采集与受控重连验证。没有真实订单，`verified_live_realized_pnl = null`。

全项目仍是 **PARTIALLY_VERIFIED**：人工批准配对为 0；严格 100 ms 更新时差的诊断配对覆盖也为 0。不能将同时健康的行情状态称为已经获得经济审批的套利覆盖。

## 实际采集

芝加哥时间 **2026-11-05 23:29:12–23:39:12 CST**；UTC `2026-11-06T05:29:12.418106622Z` 至 `2026-11-06T05:39:12.658817170Z`，实际 **600.241 秒**。发现 Kalshi 812、Polymarket 1200 个市场（有界分页采样），实际观察 4+4 个活跃市场。

保存 **7,016 条 raw 记录、18,200,843 字节**。致命错误 0。

| 指标 | Kalshi | Polymarket 国际版 |
|---|---:|---:|
| WS 行情消息帧 | 1305 | 5157 |
| WS snapshot/book 消息 | 8 | 1214 |
| WS delta/price_change 消息 | 122 | 1743 |
| 成功 REST 盘口诊断/比较响应 | 154 | 304 |
| 非预定重建 | 0 | 2 |
| 受控验证重连 | 1 | 1 |
| 断开至重开累计秒数 | 1.206 | 4.549 |
| 全部所选资产健康 WS 覆盖秒数 | 598.764 | 594.271 |

帧数包含非盘口市场事件；一帧可以携带多份资产消息，因此不等于独立盘口观察数。REST 始终不计入连续 WS 覆盖。

双方至少一个资产同时健康：**594.272 秒**。Aliens 待审候选的两边指定盘口同时健康：**594.272 秒**；其盘口更新接收时刻差 ≤100 ms 的覆盖：**0.000 秒**。人工批准配对的覆盖为 **0 秒**。

## sid、seq、clock 与恢复

Kalshi 8 份快照、122 份增量全部带 sid/seq/clock。独立按 `(connection_epoch, sid)` 检查序列，按 `(connection_epoch, market)` 检查 exchange clock：订阅序列缺口 0、clock 回退 0、delta 先于自身市场 snapshot 的次数 0、解析错误 0。本地 7016 条记录序列也连续。

第 300 秒明确记录了一次双方受控断开。Kalshi epoch 2 收到全部 4 个市场的新快照，其中 3 个市场随后观察到 delta；AELS 在重连后的剩余窗口没有观察到 delta。Polymarket epoch 4 的 8 个资产均有新 snapshot 和后续 price_change。逐条 raw 记录号见 [snapshot_recovery.csv](../runs/cross_venue/v0_1_1_authenticated_20261105_3/capture_feed_audit_verified/snapshot_recovery.csv)。断线空档没有被补成连续数据。

Polymarket 另有 2 次 REST/WS 静默窗口不一致，触发保守重建；3 次异步或旧源时间比较标记为 inconclusive。CLOB 没有连续公共交换序列，不能从本地到达连续推断实际交换漏包数为零。

## 时间与延迟限制

| 指标 ms | Kalshi | Polymarket |
|---|---:|---:|
| 盘口 source→callback 中位数 | -102.279 | -58.183 |
| 盘口 source→callback p95 | -69.488 | 29.822 |
| 本地 callback→processing p95 | 0.003473 | 0.006701 |

Kalshi 全部 130 份盘口时间差为负；Poly 2957 份中有 2604 份为负。负值原样保留。系统显示 NTP synchronized，但归档的观测 NTP offset 为 +144.494 ms；参见 [NTP 原始诊断](../runs/cross_venue/v0_1_1_authenticated_20261105_3/validation/ntp_status.txt)。这些差值包含时钟偏移和源端批处理，**不能解释为负网络延迟，也未测得 WS 网络 RTT**。没有改动系统时间或原始时间戳；回放使用 monotonic availability。

## 实际修复

1. 首两次 60 秒 smoke 已通过 signed session 收到真实快照，但每个未订阅市场的全局 Kalshi lifecycle 通知都导致客户端错误返回失败并断开。`ResearchState.cpp` 现在忽略未映射市场的 lifecycle/tick 事件，同时仍处理已观察市场的状态变化。真实失败录制保留，5 条原始认证协议样本加入回归。
2. 将连接存活与价格更新分开：同一场所 socket 上的有效 WS 消息刷新 connection liveness，保持市场更新、authority、source transport 和严格 skew 条件原样。原分析保守计为 580.881 秒双场所覆盖；修复后用同一 raw 重放为 594.272 秒。没有制造价格更新或回填空档。原分析与新分析都保留，各自固定源码哈希。
3. 在既有签名客户端上补充本地密钥兼容性验证、受控重连、源/事件/回调/处理时间、逐 epoch 恢复与独立协议审计。没有重写 WS、book store、risk 或 ledger。

本地密钥为 Ed25519 256 位，权限 0600，现有签名器本地验签通过。初始 socket 传递接收器曾超时；最终直接从用户已配置环境的 WSL 终端启动 native 子进程。Key ID、私钥和签名没有写入参数、配置、日志、Git 或审计文件；捕获在记录前拒绝检测到的认证材料。

## 回放、测试和审计

真实录制已运行 **0/100/250/500/1000 ms × 4 执行模型**。明确人工批准的 pair 为 0，因此模拟交易为 0，PnL/存活率/容量保持 null/未测；认证成功没有放宽规则、币种、数量或费用门。

两次独立显式回放的 **40 个文件逐字节一致**。独立回放审计 `LOCAL_AUDIT_PASSED`，核验 **118 个哈希**。独立协议审计 `INDEPENDENT_PROTOCOL_AUDIT_PASSED`，验证 3 个 raw 文件、7016 条到达记录、2 个订阅 epoch 和全部 122 个 delta 在自身市场 snapshot 之后。

全量构建成功。Cross-Venue **8/8 CTest 组通过**，含 **158 项原生检查**、6 项安全启动检查、3 项有界工作流证据门检查，原 V0 回归保持通过。全仓 **1030 项注册：1024 通过、1 失败、5 禁用**；失败仍是原有 `AvalancheSkewsQuotesAndRiskCutsBuyChannel`，没有修改该做市策略或弱化断言。

## 证据与复现

- [自动完整报告](../runs/cross_venue/v0_1_1_authenticated_20261105_3/report/report.md) / [结果 JSON](../runs/cross_venue/v0_1_1_authenticated_20261105_3/report/summary.json)
- [修复后 native 健康审计](../runs/cross_venue/v0_1_1_authenticated_20261105_3/capture_feed_audit_verified/summary.json) / [逐资产覆盖](../runs/cross_venue/v0_1_1_authenticated_20261105_3/capture_feed_audit_verified/asset_coverage.csv)
- [全部 20 场景 CSV](../runs/cross_venue/v0_1_1_authenticated_20261105_3/capture/analysis_verified/latency_sensitivity.csv)
- [独立协议审计](../runs/cross_venue/v0_1_1_authenticated_20261105_3/validation/independent_protocol_audit.json) / [独立回放审计](../runs/cross_venue/v0_1_1_authenticated_20261105_3/validation/independent_replay_audit.json)
- [重复回放证明](../runs/cross_venue/v0_1_1_authenticated_20261105_3/validation/determinism.json) / [完整 CTest](../runs/cross_venue/v0_1_1_authenticated_20261105_3/validation/ctest.xml)
- [完整 raw/source 清单](../runs/cross_venue/v0_1_1_authenticated_20261105_3/capture/capture_manifest.json)

```bash
# 在配置了变量的 WSL 终端发起新的有界验证；无 socket、无持久化凭证。
python3 research/cross_venue/secure_kalshi_environment.py launch

# 已有录制不再需要凭证：out 必须是新/空目录。
build/run_cross_venue_l2_backtest \
  --run-dir runs/cross_venue/v0_1_1_authenticated_20261105_3/capture \
  --capital 200000 --latencies-ms 0,100,250,500,1000 \
  --out runs/cross_venue/v0_1_1_authenticated_20261105_3/capture/another_empty_analysis

python3 research/cross_venue/audit_authenticated_protocol.py \
  --run-dir runs/cross_venue/v0_1_1_authenticated_20261105_3/capture \
  --native-audit-dir runs/cross_venue/v0_1_1_authenticated_20261105_3/capture_feed_audit_verified
```

接下来应处理时钟偏移与同步策略的可验证定义，并完成候选的人工规则、资产和数量精度审核。当前没有合格套利或真实利润结论，没有启动 24/72 小时任务，也没有启用真实下单。

官方协议：[API Keys](https://docs.kalshi.com/getting_started/api_keys)、[WS](https://docs.kalshi.com/getting_started/quick_start_websockets)、[Orderbook Updates](https://docs.kalshi.com/websockets/orderbook-updates)。

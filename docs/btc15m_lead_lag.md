# BTC 15 分钟 Lead–lag 条件价值做市

新入口为 `products/btc15m_lead_lag/LeadLagEngine`，编入现有
`engine_strategy_market_making`。输出复用 `QuoteIntent` / `CancelQuoteIntent`；
回放通过现有 `QuoteRiskEvaluator` 审批后才接受订单。
原有 Generic、BTC 5 分钟策略和 dashboard 启动行为不变。

## 已接入的行为

- UP/DOWN 共同价格锚和净库存 `q_UP - q_DOWN`；四个通道分别占用报价组。
- 两组冻结的标准化 Ridge 模型生成四个通道的条件变化预测；总体 alpha 是四者平均值。
- CALM / SHIFT / SHOCK / HALT / WAIT，阈值为 0.005 / 0.015 / 0.04；
  SHOCK/HALT 后连续安静 1500ms 才恢复，不降低恢复期缓冲。
- 通道条件价值、每份库存 0.00025 偏移、0.0025 缓冲、0.01 网格和 touch 限制。
- 先检查危险方向、偏移前后缓冲、价差容忍和 TTL，再保留旧单队列。
- continuous 与 shock_entry 候选。后者只在冲击有利侧增加风险，其他可交易状态
  只允许减仓，两个同向减仓通道共用净库存额度。
- continuous 候选中，单笔数量超过当前可减净库存时标记为 Mixed，
  不使用仅减仓豁免；真正的 ReduceOnly 目标对应消除净库存后的 Token 数量。
- 提交、撤单和未通知成交共同预留现金、Token 和 UP 等价风险；禁止裸卖。
- source / receive / execution 时间分离；公共成交发生在 source 时间，
  外部和本地特征只在 receive 时间可见。
- 25ms 发单、100ms 撤单、50ms 通知及可选 275ms 末端减仓。
  这些都是可修改的回放假设。迟到新单先接受、重新评估、再撤单。
- 公共成交先消耗显示队列，余量才能成交；成交不能超过公共量和订单剩余量；
  不从他人撤单推算队列前进。缺少报价价位的显示深度则不发单。
- 100ms / 1s / 5s 数量加权 markout、现金账本、已知/实际库存、
  已知结算 PnL 或未知结算上下界。手续费和运行成本可配置，奖励/rebate 为零。

## 模型和数据边界

原始说明未提供训练权重、预处理代码、冻结配置文件或 BTC 15 分钟数据。
因此默认 `model_path: null`，策略停单。测试里的零系数模型是显式标记的合成
fixture，运行工具必须加 `--allow-synthetic-model` 才能使用，不能作为收益验证。

特征顺序如下，数组长度必须为 13：

```text
btc_100ms, btc_250ms, btc_1s, btc_5s, imbalance_250ms,
pm_500ms, pm_1s, pm_5s, pair_gap, token_spread,
anchor, tau_seconds, token_deviation
```

此实现明确采用 `recv-clock-rv1s-60s-v1` 预处理：
按接收时钟取历史价格，使用最多 60 个不重叠的一秒 BTC log return 的样本标准差，
至少需要 10 个收益样本且方差非零。sigma 单位为每 sqrt(second)，tau 单位为秒。
共同价格锚的历史变化也按接收时钟计算，历史取点距离目标时间不得超过
`max_data_age_ms`。BTC 特征的截断值由模型文件冻结；示例为 0.25。
250ms 成交失衡使用 BTC 成交数量和 `buyer_is_maker`。
两边偏离为 `UP_mid - (1 - DOWN_mid)`；spread 是当前 Token 的 spread；
token_deviation 使用该 Token 的 UP 等价 mid 减共同锚。

**这些预处理细节是本次接入所定义的接口，不能假定与原研究实现一致。**
已有模型必须确认预处理兼容后导出；不兼容时需要适配预处理或重新训练。
程序不会在测试窗口拟合或修改模型。训练截止时间必须不晚于合约开始时间。
只接受 BTCUSDT、900000ms 的 UP/DOWN 成对合约。

模型文件示意（两个模型均需完整数组）：

```json
{
  "schema": "btc15m-lead-lag-v1",
  "symbol": "BTCUSDT",
  "duration_ms": 900000,
  "preprocessing": "recv-clock-rv1s-60s-v1",
  "feature_names": ["btc_100ms", "btc_250ms", "btc_1s", "btc_5s", "imbalance_250ms", "pm_500ms", "pm_1s", "pm_5s", "pair_gap", "token_spread", "anchor", "tau_seconds", "token_deviation"],
  "trained_until_ms": 0,
  "synthetic": false,
  "pm_only": false,
  "external_feature_clip": 0.25,
  "plus": {"mean": [0,0,0,0,0,0,0,0,0,0,0,0,0], "scale": [1,1,1,1,1,1,1,1,1,1,1,1,1], "coefficients": [0,0,0,0,0,0,0,0,0,0,0,0,0], "intercept": 0},
  "minus": {"mean": [0,0,0,0,0,0,0,0,0,0,0,0,0], "scale": [1,1,1,1,1,1,1,1,1,1,1,1,1], "coefficients": [0,0,0,0,0,0,0,0,0,0,0,0,0], "intercept": 0}
}
```

上述零数组只解释结构，不是已训练模型。PM-only 对照应独立拟合并设置
`pm_only: true`，前五项系数必须为零；不能只把完整模型的外部系数清零来声称
完成对照训练。模型文件原文哈希随报价输出。

## 运行

复制 `config/btc15m_lead_lag.example.json`，填入真实合约和冻结模型路径。
价格单位为 millionth dollar：`10000 = $0.01`；数量单位为整份。
`initial_cash_tick` 是购入初始配对库存后剩余现金，初始资本按现金加配对库存
成本（每对 $1）计入。末端减仓仅通过显式 `terminal_reduce_request_ms` 开启，
随后停止新 Maker 报价，并在延迟结束、全部取消和成交通知完成后按可见买盘
深度出售净多的 Token；不产生信号驱动 taker 开仓。

```bash
cmake -S . -B build
cmake --build build --target run_btc15m_lead_lag_replay btc15m_lead_lag_tests -j4
ctest --test-dir build -R Btc15mLeadLag --output-on-failure
build/run_btc15m_lead_lag_replay \
  --config config/btc15m_lead_lag.example.json \
  --model /path/to/frozen-model.json \
  --events /path/to/normalized-events.jsonl \
  --trace /tmp/btc15m-lead-lag-trace.jsonl
```

已知结果时追加 `--settlement up` 或 `--settlement down`。输出中的上下界反映
剩余库存结算风险，不是统计置信区间。

事件输入为标准化 JSONL，允许行未排序；事件引擎按时间、优先级、输入序号排序。
`books` 中两份快照必须是同一可见时刻的成对状态，保留各自新鲜度；当前命令行
格式支持各 Token 内独立的 `recv_ms`，缺省时使用外层 `recv_ms`；
上游应在缺失/陈旧一边时标记 `usable: false`，不能用轮询时间刷新旧盘口。
`event_ms` 是成对价格快照的来源时间，markout 只使用来源时间达到对应 horizon
后的快照，缺失结果保留 null。

```jsonl
{"type":"agg_trade","event_ms":90000,"recv_ms":90025,"price":70000.5,"quantity":0.12,"buyer_is_maker":false}
{"type":"books","event_ms":90000,"recv_ms":90020,"up":{"usable":true,"bids":[[490000,20]],"asks":[[510000,20]]},"down":{"usable":true,"bids":[[490000,20]],"asks":[[510000,20]]}}
{"type":"public_trade","event_ms":102000,"recv_ms":102030,"token":"up","taker_buy":true,"price_tick":510000,"quantity_lots":25}
```

同毫秒先发生公共成交，随后可见行情、发单生效、撤单生效、成交通知、策略决策。
因此不能用同毫秒撤单倒推避开成交。没有对手订单时间线或 FIFO 还原，队列模型
是保守反事实近似；初始队列来自提交时可见快照，不估计自身冲击。

直接嵌入运行时可调用 `on_agg_trade`、`on_books`、`decide`，将输出交给现有
风控和执行层。`Input::outstanding` 必须包括所有在途订单和未通知成交的预留量；
撤单发送后不可立即移除预留。回放提供这一生命周期的参考实现。

PMXT 公共历史数据适配、独立训练和冻结模型的样本外比较见
[PMXT 回测说明](pmxt_btc15m_backtest.md)。公共 taker 成交匹配是自有 Maker
成交条件的代理，完整订单 FIFO、自身冲击和 Chainlink/TWAP 定价未还原。
研究回放不会切换实盘进程。

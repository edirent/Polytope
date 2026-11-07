# PMXT v2 BTC15m 回测

> 当前审计状态：已使用的四五月窗口全部为 development；旧 reused/confirmation 名称仅保留历史路径含义。旧 markout 使用合成锚及 first-after 规则，不能当作严格 token 标签。参见 [Re-entry 审计](pmxt_reentry_audit.md)。

`research/pmxt_btc15m_backtest.py` 将 PMXT 的真实公共事件转换到
`run_btc15m_lead_lag_replay`，使用现有报价风控、延迟、队列和现金/库存账本。
事件有来源时间和 PMXT exporter 接收时间。后者不是本地实盘客户端的实测接收时间。

固定窗口为 UTC 2026-04-17 00:00 至 2026-05-01 00:00（不含），共 336 小时、
1344 个 BTC 15 分钟 UP/DOWN 市场。4 月 17–20 日的 384 个市场用于训练；
4 月 21–30 日的 960 个市场用于样本外测试。训练参数不会根据测试收益调整。

## 输入和覆盖审计

先对每个小时的真实 R2 对象执行 HEAD；索引缺口或索引暂时无法访问不作为
连续性证据。另取 4 月 16 日订阅种子数据和 5 月 1 日 00 时尾部数据。
逐市场用公开 Gamma slug 验证 condition ID、UP/DOWN token、5 股下单最小量和
已结算的二元结果，结果只在回放结束后的会计结算中使用。

Parquet 使用 market 范围 row-group 统计加 HTTP byte range 读取，再执行精确
condition ID 过滤，缓存 ETag、schema、选中行数、时钟延迟分位数和文件 SHA256。
记录订阅初始两小时、合约前一小时以及交易和尾部小时，不声称下载了全部市场
的全部订单历史。使用公开 Binance BTCUSDT spot aggTrades 日文件，验证官方
SHA256；2026 年现货来源时间为微秒，转换为毫秒。Binance 不含实测接收时钟。

盘口数组不假定排序。`price_change.size` 是价位更新后的绝对量，零量删除。
同一接收毫秒按来源时间排序；这些事件只有在接收后才可见。超过一小时的数据
间隔清空未知深度。best bid/ask 只用于删除不可能仍存在的更优价位，不能凭
best bid/ask 创造数量。可从明确的 price_change 重建部分价位；报价要求两个
真实已知 touch、与声明的 best bid/ask 一致、各自不超过 1500ms 的新鲜度，且
实际报价价位有已知数量。仅导出每侧前 16 个已知价位。

50ms 轮询导出成对状态，保留各 Token 原始 `recv_ms`，额外轮询延迟为 0–50ms。
公共成交按来源时间进入撮合，完全相同的事件去重；成交量向下取整到整股。
没有公共逐笔订单 FIFO，队列使用提交时已知显示量，且只由公共成交推进，
他人撤单不推进自己的队列。该近似会少算成交，也不能证明真实可成交性。

对象覆盖和实际可交易覆盖分别输出，不能把 336 个文件可下载等同于连续可用
盘口。`coverage.json` 记录逐市场的有效盘口、特征就绪、缺失标签和舍弃量。

## 冻结训练与比较

用公共 taker 严格触及来源时间上更早的 best bid/ask 作为代理成交样本。
取该成交之前 100ms 已可见的特征，以来源时间成交后 1 秒的共同价格锚减
特征锚作为标签；缺失、超过 1500ms 陈旧或不能严格匹配的样本不训练。
训练截止前必须已包含完整标签 horizon，防止跨入测试区间。

每个合约的样本总权重为 1，再分为两种 UP 等价风险方向；分别标准化并拟合
Ridge（预先固定惩罚 1.0）。外部模型使用 13 项特征，PM-only 独立拟合后 8 项，
前五项系数严格为零。训练不足的合约保留审计记录；测试合约一律保留，
包括无报价、无成交和低覆盖合约。模型原文 SHA256 写入每项回放结果。

四个固定比较配置为：

| 配置 | 模型 | 撤单延迟 | 候选 |
|---|---|---:|---|
| external | 外部 + PM | 100ms | continuous |
| pm_only_common | 独立 PM-only | 100ms | continuous |
| external_cancel200 | 与 external 相同 | 200ms | continuous |
| shock_entry | 与 external 相同 | 100ms | shock_entry |

PM-only 主对照也要求外部数据达到就绪条件，控制数据可用性差异，外部系数
仍为零；这不是 PM-only 自身最佳部署方式。两种模型的独立状态机可能造成
后续实际报价时间不同。默认固定 Binance 接收延迟 20ms、发单 25ms、成交
通知 50ms；这些均为假设，PMXT 接收延迟则来自文件。

下单网格 $0.01、每单 5 股，低于 5 股的减仓不发单，净库存上限 50 股，
每 Token 上限 100 股。每个市场独立以购入 25 对后剩余 $100 现金启动，
初始资本 $125；配对库存成本按每对 $1 扣除，不把配对库存当免费收益。

主比较保持纯 Maker，未启动可选期末 Taker 减仓，残留库存按实际 UP/DOWN
结算结果计入。结果分解现金变化和残留获胜 Token 相对初始 25 股的变化，
不能把结算盈利都解释成价差收入。Maker 费按零设置；rebate 和运营成本为零，
所以结果是补贴为零、运营成本前的模拟交易 PnL。没有用当前 taker 费率代替
历史费率；启用 terminal_reduce 时须单独适配历史非线性费率。

到期时显式停止报价并发出撤单，撤单仍需 100ms 或 200ms 才生效。额外保留
到期后 5 秒的真实公共成交，让尚未生效的到期撤单继续承担被成交风险；这些
尾部成交不进入训练样本。到期后的通知继续更新账本，直到预留全部释放。
报价 TTL 同样触发有延迟的撤单请求，不假定交易所提供即时自动到期。

submit 时显示的队列和 post-only 通过状态仍是模拟假设，没有还原真实提交
到交易所时的完整订单簿、GTD 服务行为或自身冲击。结果不能证明稳定实盘收益。

## 复现

```bash
python3 -m venv .local/pmxt-venv
.local/pmxt-venv/bin/pip install -r research/requirements-pmxt.txt
cmake --build build --target run_btc15m_lead_lag_replay btc15m_lead_lag_tests -j4
ctest --test-dir build -R 'Btc15mLeadLag|QuoteRiskEvaluator' --output-on-failure
.local/pmxt-venv/bin/python -m unittest discover -s tests/research -p test_pmxt_btc15m_adapter.py
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py audit --workers 8
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py fetch-pmxt --workers 8
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py fetch-binance --workers 4
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py shard
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py normalize --workers 14
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py repair-tail --workers 4
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py fit
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py backtest --workers 6 --profiles external pm_only_common external_cancel200 shock_entry
.local/pmxt-venv/bin/python research/pmxt_btc15m_backtest.py report
```

输出默认放在 `runs/pmxt_btc15m_20260417_20260430/`，包括 coverage、模型、
逐市场 summary、首个样本外市场 trace 和总 `results.json`。缓存结果模型或
配置变化时会拒绝复用。重新改变预处理应使用新的 `--out` 目录，避免旧缓存。

数据与格式依据：[PMXT v2 数据说明](https://archive.pmxt.dev/docs/v2-data-overview)、
[PMXT 索引](https://archive.pmxt.dev/Polymarket/v2)、
[官方采集器](https://github.com/pmxt-dev/polymarket-orderbook-collector)、
[Binance 公共数据](https://github.com/binance/binance-public-data)、
[Polymarket 实时事件](https://docs.polymarket.com/market-data/realtime-data)、
[Polymarket 当前费用文档](https://docs.polymarket.com/trading/fees)。

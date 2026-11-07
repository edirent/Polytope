# 固定 shock-entry 的扩展验证

> 当前审计状态：已使用的四五月窗口全部为 development；旧 reused/confirmation 名称仅保留历史路径含义。旧 markout 使用合成锚及 first-after 规则，不能当作严格 token 标签。参见 [Re-entry 审计](pmxt_reentry_audit.md)。

从首轮对照的 `shock_entry` 配置冻结模型与策略：Ridge 权重仍来自 UTC
2026-04-17–20，训练截止 2026-04-21，原文 SHA256 为
`e3073ad25980120b2a8e3f0017cc16f4c2c232fbacae9ef993cf6da516ced9fb`。
冲击阈值仍为 0.015、HALT 为 0.04、恢复安静期 1500ms，报价缓冲、库存偏移、
数量和风控上限全部沿用原配置。本实验没有重新拟合或调优这些参数。

4 月 21–30 日已用于本次候选选择，复用这 960 个市场的压力测试不再作为
独立确认。新增 UTC 5 月 1–7 日的 672 个市场未参与选择，用于确认冻结候选。
新窗口先逐小时 HEAD 审计 PMXT 对象，再验证 condition/token/最小数量/二元
结算结果；订阅种子、实时小时与到期尾部全部到齐后才标准化对应市场。

| 测试配置 | BTC 接收 ms | 发单 ms | 撤单 ms | 成交通知 ms | 显示队列倍数 |
|---|---:|---:|---:|---:|---:|
| baseline | 20 | 25 | 100 | 50 | 1 |
| cancel200 | 20 | 25 | 200 | 50 | 1 |
| cancel500 | 20 | 25 | 500 | 50 | 1 |
| btc100 | 100 | 25 | 100 | 50 | 1 |
| btc250 | 250 | 25 | 100 | 50 | 1 |
| submit100 | 20 | 100 | 100 | 50 | 1 |
| notice200 | 20 | 25 | 100 | 200 | 1 |
| queue2 | 20 | 25 | 100 | 50 | 2 |
| joint | 100 | 100 | 200 | 200 | 2 |

复用窗口测试全部九组；新窗口运行 baseline、cancel200 和 joint 三组。
所有场景均固定 `candidate=shock_entry` 和原模型，而不是把全天强行设为 SHOCK。
原有 CALM/SHIFT/SHOCK/HALT/WAIT 状态机继续根据固定模型运作；仅在有利冲击侧
增加风险，其他可交易状态允许按原规则减仓。

`btc_receive_delay_ms` 只将 BTC aggTrade 的可见时间设为其来源时间加假设延迟，
不更改 PMXT 的 `timestamp` / `timestamp_received`，也不修改公共成交时间。
CLI 不提供该字段时保留输入原始接收时间。新基准逐市场校验原 shock 结果
完全相同；模型、执行程序哈希、固定参数、实际/已知账本和所有预留量均检查。

结果包含真实结算残留库存，未启用期末 Taker；初始资本、Maker 费零假设、
补贴零和运营成本零沿用首轮。另给出每市场 5/10/25/50 美分运营成本的
事后扣减，只反映收益余量，不重新模拟现金约束。队列仍是公共成交近似，
无法证明真实 FIFO 或交易所提交时可成交性。逐日 bootstrap 仅有 7 或 10 个
日块，区间用作描述，不能证明长期稳定收益。

复现新增数据准备：

```python
from pathlib import Path
import pmxt_btc15m_backtest as data
out = Path('runs/pmxt_shock_fixed_20260501_20260507')
data.audit_inputs(out, 8, '2026-05-01', '2026-05-08')
data.fetch_hours(out, 8)
data.fetch_binance(out, 4, '2026-05-01', '2026-05-08')
```

从项目根目录运行，Python 导入时加入 `research` 至 `PYTHONPATH`，依赖与首轮相同。

```bash
cmake --build build --target run_btc15m_lead_lag_replay -j4
.local/pmxt-venv/bin/python research/pmxt_shock_fixed_tests.py freeze
.local/pmxt-venv/bin/python research/pmxt_shock_fixed_tests.py prepare --workers 10
.local/pmxt-venv/bin/python research/pmxt_shock_fixed_tests.py reused --workers 20
.local/pmxt-venv/bin/python research/pmxt_shock_fixed_tests.py confirmation --workers 20
.local/pmxt-venv/bin/python research/pmxt_shock_fixed_tests.py report
```

输出 `runs/pmxt_shock_fixed_tests_20261006/`，冻结清单与模型独立保存。
输入和方法详见 [PMXT 回测说明](pmxt_btc15m_backtest.md)；BTC 格式与校验依据
[Binance 官方公共数据文档](https://github.com/binance/binance-public-data)。

逐日 markout、closed/paired 与期末方向库存的收益拆分，以及两个窗口的
状态分布和 shock 后延续诊断，见 [冻结策略收益归因](pmxt_shock_diagnostics.md)。
仅实际模拟成交的 shock 时间分桶、挂单风险集分母和成交条件 markout，见
[Shock age 成交条件实验](pmxt_fill_conditional.md)。

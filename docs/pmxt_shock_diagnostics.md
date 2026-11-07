# 冻结 shock 策略的收益归因与窗口比较

> 当前审计状态：已使用的四五月窗口全部为 development；旧 reused/confirmation 名称仅保留历史路径含义。旧 markout 使用合成锚及 first-after 规则，不能当作严格 token 标签。参见 [Re-entry 审计](pmxt_reentry_audit.md)。

该诊断沿用已经冻结的模型、策略配置和执行程序。旧窗口为 UTC 2026-04-21–30
的 960 个市场；新窗口为 UTC 2026-05-01–07 的 672 个市场。旧窗口取 baseline，
新窗口取 baseline、cancel200、joint，共 2,976 个原结果的逐笔重放。
每次要求全部 stdout summary 字段与原结果完全相同，再校验逐笔现金、库存、
结算收益和三个期限的成交量加权 markout。无新增交易阈值或模型训练。

## 逐日收益与 markout

日期按市场开始时间的 UTC 日划分。1 秒 markout 使用存在有效 1 秒后报价的
成交股数加权，不对每个市场的平均 markout 再简单平均。缺失后续价格不填零。
报告同时给出有效 markout 股数、逐日 closed/paired 与 terminal 分项。

## 收益拆分

初始现金 $100、25 对 UP/DOWN 库存构成 $125 初始价值，方向暴露为零。
定义 `C* = cash + DOWN`、`N = UP - DOWN`。因为一对互补 token 的确定
结算价值为 $1，有效组合价值为 `C* + N * Y`，其中 UP 赢家时 `Y=1`，
否则 `Y=0`。

| 原成交 | 等价 UP 方向 | 等价成交价 |
|---|---|---|
| Buy UP | 买 | p |
| Sell DOWN | 买 | 1−p |
| Sell UP | 卖 | p |
| Buy DOWN | 卖 | 1−p |

按发生时间对相反方向成交进行 FIFO 匹配。每个闭合数量贡献等价卖价减买价，
不使用最终赢家选择配对。配对涵盖同 token 买卖往返、互补 token 买入对、
互补 token 卖出对。剩余等价方向暴露保留自己的入场成本，并与实际结算结果
比较。因此每个市场都有精确恒等式：

`settlement PnL = closed/paired PnL + terminal directional PnL`。

这避免了把期末 winning-token 数量本身叫作利润，也避免了给初始每个单腿
任意分配 $0.50 成本。成本配对存在方法依赖，因此同时计算 LIFO 与移动平均
成本分项，并核对它们的总和不变。closed/paired 价差包含持仓期间可能发生的
方向性变动，不能直接把全部已实现收益解释为 spread capture。

## 状态分布与持续性

状态比较统一使用 baseline 每 50ms 网格，排除因公共成交到达额外触发的
决策，以避免窗口成交密度影响采样。仅 ready 时的 alpha、盘口与时间状态
进入主要分布；同时报告 ready 覆盖率和逐日分布。分位数为 p10/p50/p90/p99。

BTC realized volatility 使用策略自身的滚动样本标准差：最多 60 个不重叠
1 秒 log return，遇到不可用历史点停止，至少 10 个连续有效样本，历史价格
最大允许 1500ms 过旧。报告单位为 bps/√秒。

Gamma 原始缓存的 `eventMetadata.priceToBeat` 提供部分市场官方 strike。
报告单独审计元数据覆盖和 finalPrice 与原赢家的一致性。动态 BTC 价格是
Binance BTCUSDT；与官方 Chainlink BTC/USD strike 比较含跨源基差，不能声称
拥有完整 Chainlink 价格路径。缺失官方 strike 不用代理填充。另列市场开始前
最后 Binance 成交价（最多早 1500ms）的距离代理，作为第二种距离口径；
开盘附近没有足够新鲜成交的市场也记为缺失，单独报告有效覆盖。

Shock onset 要求相邻两个有效网格从 `|alpha|<0.015` 穿越到 `>=0.015`；
缺失后已经超阈值的第一个点不当作已知穿越。相邻保留事件至少间隔 5 秒，
每个事件距到期至少 10 秒，未来值不跨市场。报告 shock 后 250ms、1s、5s、
10s 的 alpha 同号率、低于 CALM 的比例，以及按 onset alpha 方向计算的
PM anchor 变化和 BTC log return。PM 后续价格只要求可用，不要求未来策略
再次 ready。总 shock 与可报价 SHOCK（不含 `|alpha|>=0.04`）分别列出。

日块 bootstrap 使用旧窗口 10 日、新窗口 7 日，按日重采样并保留当日样本
权重，给出新减旧的描述区间。不能把相关的 50ms 网格或相邻 shock 当成独立
观察。另按 tau 四档、旧窗口波动率四分位、alpha 强度两档和正负方向分层，
在共同支持上按旧事件构成标准化新窗口结果；用于检查 mix shift 与条件
延续变化，不作为因果识别或重新调参。

## 复现

在原冻结回放全部完成后，从仓库根目录运行：

```bash
.local/pmxt-venv/bin/python -m pip install -r research/requirements-pmxt-diagnostics.txt
.local/pmxt-venv/bin/python research/pmxt_shock_diagnostics.py analyze --workers 16
.local/pmxt-venv/bin/python research/pmxt_shock_diagnostics.py report
.local/pmxt-venv/bin/python research/pmxt_shock_regime_comparison.py strikes --workers 8
.local/pmxt-venv/bin/python research/pmxt_shock_regime_comparison.py report
.local/pmxt-venv/bin/python research/plot_pmxt_shock_diagnostics.py
```

输出位于 `runs/pmxt_shock_fixed_tests_20261006/diagnostics/`。原实验文件保留；
新目录保存逐市场压缩成交、固定网格 NPZ、事件响应、方法清单、完整机器结果
和 PNG/SVG 研究图表。诊断脚本存档保留实际计算规则，报表修复单独记录源哈希。
详见 [固定策略实验](pmxt_shock_fixed_tests.md)。

# Shock age：实际成交条件实验

> 当前审计状态：已使用的四五月窗口全部为 development；旧 reused/confirmation 名称仅保留历史路径含义。旧 markout 使用合成锚及 first-after 规则，不能当作严格 token 标签。参见 [Re-entry 审计](pmxt_reentry_audit.md)。

只沿用已冻结 baseline，分别在 UTC 4 月 21–30 日的 960 个 reused 市场与
5 月 1–7 日的 672 个 confirmation 市场运行。同一组时间分桶同时给出
fill probability、100ms/1s/5s markout 与 spread，不改变策略或延迟参数。

## Shock 与事件归属

Shock 起点采用实际回放决策时间：上一决策 ready，当前 ready，决策间隔
不超过 1500ms，`|alpha|` 从 `<0.015` 穿越到 `>=0.015`。含达到 HALT
强度的冲击，并另外记录可报价强度的事件数量；不使用前次分析的 50ms
抽样或 5 秒去簇。缺失后首次发现已经超阈值不当作已知穿越。

每个成交只归属一次。当前处于最近一次 shock 后 0–5 秒时，按该 onset
分桶；下一次 onset 重置年龄。shock 前是下一次 onset 前 1 秒，且只用
没有落入前一个 shock 后 5 秒的部分，以免同一成交重复计算。
其他成交记为窗口外，不强行填进 pre 桶。

回放实际执行序号处理毫秒内排序。公共成交在来源时间发生；若它与
机器人发现 shock 同毫秒但先执行，则机器人当时还没发现信号，不能
将它认作发现 shock 之后的成交。

## 分母与条件收益

风险样本为 `订单 × shock × 时间桶`。只有已经在模拟交易所生效且尚有
剩余数量的挂单与桶存在交集，才进入分母。等待发单确认的订单不计入；
撤单尚未生效的订单仍计入；部分成交之后的剩余挂单仍有效。

`Fill probability = 桶内至少一次实际成交的风险样本数 / 全部风险样本数`。

一张订单在一个桶有多笔部分成交时，概率的分子仍只计一次。它跨多个
桶时则会分别进入对应风险集。该概率依赖区间长度和实际撤单造成的暴露
时间，不是同一固定期限的 hazard；报告另列每有效挂单秒的成交次数。

三个 markout 只使用实际模拟成交，按成交股数加权，保留原回放 PM source
horizon 标签。缺失未来价格不记为零，并逐桶给出有效股数。
Spread 只使用成交前本方已可见的对应 token bid–ask 差，按成交股数加权；
不可用、交叉或超过 1500ms 过旧则记缺失。这不是未知的交易所真实实时
spread，也不是所有 shock 事件的 spread 平均。

## 隔离观察与核验

原冻结 CLI 缺少实际订单生命周期和成交关联 ID。构建脚本复制原 replay
header/source/CLI 到实验目录，仅补充生效、撤单生效、成交、shock 起点的
观察记录，单独编译观察程序。原策略源文件、原回放源文件与冻结二进制
保持原样。观察程序复用现有静态库，记录完整编译命令和源文件哈希。

每个市场都要求观察副本的全部 summary 字段（包括 output hash）与原结果
相同，并逐笔核对时间、渠道、价格、数量、撤单等待标志和三个 markout。
这项核验通过后，才用观察日志的订单 ID、实际活跃区间和盘口计算分桶。

```bash
.local/pmxt-venv/bin/python research/build_pmxt_fill_observer.py
.local/pmxt-venv/bin/python research/pmxt_fill_conditional.py run --workers 24
.local/pmxt-venv/bin/python research/pmxt_fill_conditional.py report
```

输出目录为 `runs/pmxt_shock_fixed_tests_20261006/fill_conditional/`，包含逐市场
压缩观察记录、风险集分母、逐桶有效数量、每日表、方法清单和结果汇总。
历史结果与假设见 [冻结 shock 实验](pmxt_shock_fixed_tests.md)。

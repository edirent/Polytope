# BTC15m Re-entry：回放审计与推进门槛

本阶段按任务书先完成代码审计和少量市场诊断。审计结论为 **不通过**，因此本版没有批量 shadow 训练集、价值模型或策略升级结果。
这是基础设施和数据可识别性的结论，不是策略已被证明亏损。原模型、二进制和已发布结果作为 legacy 保留。

所有已使用的四五月市场现在统一归入 **development**。旧目录名 `reused` / `confirmation` 仅用于定位历史文件，不代表当前独立确认集。
新模型 train/validation/test 时间尚未冻结；不得用这些开发样本再次宣称独立确认。先通过下面的审计门槛，再预注册未使用测试窗口和判定标准。

## 代码审计表

| 模块 / 代码位置 | 当前实现与缺陷 | 本版处理 / 待修内容 | 对收益可能的偏差 | 无法确定部分 |
|---|---|---|---|---|
| 行情读取：[normalize_market](../research/pmxt_btc15m_backtest.py#L300) | 原始 L2/public trade 存在，但送入策略的 book 是 50ms 重建快照；hourly HEAD/ETag 只能证明文件覆盖；长空白后深度重建不完整 | 本版直接读取 Parquet 为标签与到达诊断建立逐事件 source/received 视图；批量 shadow 必须接入 raw execution，不得沿用快照判断 | 可能漏掉瞬时跨价/撤单/深度变化；方向不确定 | 没有 collector 连线状态、序列连续性或订单 ID；短空白可能安静也可能缺失 |
| 时间：[add_public_trade / add_books](../replay/market_making/LeadLagReplay.cpp#L56) | 公共成交按 source 执行；books 到 received 才更新；配对快照的 event_ms 是两 token 来源时间的 **min**，只保留全局 source 水位 | 分离逐 token 时间轴；记录来源和接收年龄；对同毫秒前/后到达顺序及反转原始行顺序作敏感性；不把原始行序当 exchange 真值 | 跨时钟、同毫秒顺序影响 fill/cancel，方向不确定 | exporter 不是客户端；BTC received=source+20ms 为 assumed；跨源时钟未实测校准 |
| 行情新鲜度：[healthy](../engine/strategy/market_making/products/btc15m_lead_lag/LeadLagEngine.cpp#L20) | 现有1500ms健康检查使用收到报价后的年龄，没有逐 token 的 source-age 拒绝；新收到的积压旧行情可被视为新鲜 | 本版标签同时限制 source/received 年龄；完整引擎还需增加逐 token 来源年龄，不能只凭刚收到就报价 | 可增加基于陈旧状态的 exposure 和错误 lead-lag；PnL方向不能事先保证 | source/exporter clock偏差和真实延迟不能完全分离；May3试验市场接收lag p95=6595ms，不能当正常客户端网络时延 |
| 特征：[LeadLagEngine::on_agg_trade / on_books / features](../engine/strategy/market_making/products/btc15m_lead_lag/LeadLagEngine.cpp#L132)，[build_features](../research/pmxt_btc15m_backtest.py#L249) | receive-time asof 历史，检查 future data，收益率、成交失衡、RV 只用当时可见输入；当前查阅未发现显式未来输入；原 ridge 使用既有训练标签 | 保持原 predictor、特征和 shock 冻结；本版不重训。新模型只能读取 decision-time 特征，新增 OFI 无 CEX L2 时为缺失，不能把成交失衡改名 OFI | 20ms 接收假设可能夸大实际外部领先；无法保证所有客户端延迟 | 只有 Binance spot aggTrades；与 Chainlink 结算参考存在 basis；不是客户端 L2 数据 |
| 订单到达：[decide](../replay/market_making/LeadLagReplay.cpp#L106) | 报价风险检查在提交时；25ms 后无条件 active，未在到达 source 行情检查 post-only | 新 `arrival_check` 对 source book 重查跨价与未知队列；用旧订单做诊断，**尚未替换生产/冻结回放器**。后续必须集成独立 source book 和拒单释放/通知，重新生成整个策略分支 | 允许本应拒单的 maker exposure，填充率偏高；PnL 方向不保证 | 延迟为 assumed；真实 exchange 拒单时间、回复和 matching 顺序未知 |
| 生命周期：[request_cancel / cancel_effective / fill](../replay/market_making/LeadLagReplay.cpp#L86) | cancel-pending 保持 active；部分成交减少 actual_remaining；通知后释放已成交预留；TTL 也付 cancel latency；未 ack 撤单从 ack 后计时 | 已有处理保留；队列 primitive 验证 arrival/partial/cancel 边界。现有 observer 缺 submit/cancel client send/reply 的完整字段；不能拿它直接当 shadow lifecycle | 把撤单请求直接等同停止匹配会高估防御；当前没有这个错误。未 ack 延后规则可能增加 exposure | 真实停止匹配与客户端回复只能作为独立假设，不能写成 measured |
| 初始队列：[decide](../replay/market_making/LeadLagReplay.cpp#L147) | displayed qty 在 decision 初始化并在 ack 沿用，截取 top16；价格缺失时加总为0，未表达未知 | 原始 L2 在 arrival 重查数量；只有全 snapshot 或该价位绝对数量记录才视为已知，未知 null；记录旧提交量与到达量。仍需集成到完整执行引擎 | 少算 arrival queue 可高估 fill；排队量增加/减少均可能发生，PnL 方向不确定 | FIFO 前方占比、隐藏量、counterfactual 插入后路径未知 |
| 队列更新与成交：[trade](../replay/market_making/LeadLagReplay.cpp#L200)，[normalize_market](../research/pmxt_btc15m_backtest.py#L376) | 同 token 反向 taker public volume 先消耗 ahead 再 fill；不以 book shrink 计 fill，避免双扣；public qty floor 为整数。不能确认 UP/DOWN complementary 路径 | Decimal primitive 保留小数；conservative 不推进未知撤单；optimistic **仅**推进有独立归因、扣除已记 trade 的撤单。book decrease/increase 本身均不推进位置。缺归因时两情景可以一致，不能编造乐观增益 | floor 降低 volume/fill，但筛选后 PnL 方向不确定；忽略真实前方撤单低估容量；匹配路径不明可能过算或漏算 | 无 order/match ID 和 cancel identity；同 hash 多 token 只能提示，不能完整恢复路径。尚未完成 raw 消耗与匹配验证 |
| 标签：[update_marks](../replay/market_making/LeadLagReplay.cpp#L222) | UP/DOWN 合成 anchor 和 UP-equivalent price；取 source 到 horizon 后第一条健康快照，延后无限制；并非任务书要求的 token midpoint S/A/E | 新 `label` 用同 token、买+卖−、严格 pre-fill 可见价与 bounded asof future；100/1000/5000ms；保留 S/A/E、每 partial D、年龄、invalid reason、USD/share 与 cents/share；source-only 单列敏感性。旧回放输出不覆盖 | pair vs token 或迟到终点可改变符号/大小，方向不确定；不能继续称严格100ms标签 | 无客户端 receive；本版100ms source/receive 年龄上限非常严格，低覆盖不能用补零修复 |
| 未成交标签：[order_label](../research/pmxt_replay_audit.py) | legacy 数据没有候选级完整观察证明，旧结果是原策略实际 simulated fills | 完整观察的无成交 D 才可为0；unknown 或任一 partial label 缺失 => order D null。主标签只用 actual simulated fill，未来价格变化不构造额外成交 | 缺失记0可能虚构安全/低尾部风险 | 需要覆盖政策、连接/缺口证明或明确限定的 archive-observed 情景；不能直接拿小时齐全充当无成交证明 |
| 收益：[run / terminal_reduce](../replay/market_making/LeadLagReplay.cpp#L243)，[收益归因](../research/pmxt_shock_diagnostics.py#L39) | actual/known 分开；paired seed 成本计入；现金与结算库存只算一次；FIFO/LIFO 分解可对账；markout 未加进 PnL；原基准无 terminal taker exit | 既有拆分保留 legacy；新研究必须另冻结可执行深度退出、余量 valuation、settlement 单列、费用、预留与资本使用。闭合方向 round trip 不等于纯 spread capture | 依赖 terminal winner 会混入方向下注；只按 mid 估退出通常过高；旧账本未发现 markout/spread 重复加总 | 历史 feeSchedule 在缓存 Gamma 可见，但缓存是事后 metadata，不能证明全历史规则；真实 rebate 无 receipts，主结果不能加 rebate |
| 数据切分：[旧 freeze / report](../research/pmxt_shock_fixed_tests.py) | 原目录使用 reused/confirmation 名称，窗口已影响本轮设计 | 新 manifest 覆盖数据角色为 development；只选5个诊断 fixture，不训练、不注册虚假的 holdout | 重复使用窗口会高估模型确认可信度 | 未使用的新窗口尚未选择；完整市场 split/purge 必须覆盖暴露+cancel+最长评估/exit |

上表标出尚未集成的修复，不能将 helper 单元测试通过解释为完整回放器已通过。

## 本版验证范围

[审计代码](../research/pmxt_replay_audit.py) 沿用既有 Parquet、配置、模型、二进制和 observer 记录，不重写报价框架。
选旧/新窗口第一个有成交市场，加 May3/4/7 成交数最多市场；选择利用已知开发结果，只为覆盖缺陷，绝非样本外盈利检验。

每个市场使用冻结二进制补齐完整 trace，逐个 stdout summary 字段与原结果比较。原始事件附 raw_id，Parquet、配置、summary、trace、observer、Gamma 的 SHA256 记录在市场 summary。
五个市场全部通过原结果重现后，再审计其订单的 source-clock arrival 与 token 标签；标签只重算诊断，不删改旧成交或重新估算收益。
同毫秒前/后 arrival 比较包括状态、数量和时钟年龄，所以“敏感订单数”不等同于拒单反转数。
所选May3市场的 source→exporter 接收 lag p50=360ms、p95=6595ms，明显高于其他四个试验市场；这需要在策略新鲜度与窗口比较中排查。选样利用开发结果，不能据五个市场证明整个五月延迟分布或负 markout 的因果来源。

新 helper 的10项测试覆盖：买卖符号与 S+A=E、partial 数量加权、strict pre/asof future、source/receive 年龄、关闭/缺失/无效报价、post-only、未知深度、撤单边界、小数成交、两队列情景、trade/shrink 防双计、补单不排前、unknown D 和同毫秒顺序。

```bash
.local/pmxt-venv/bin/python -m unittest discover -s tests/research -p 'test_pmxt*.py'
.local/pmxt-venv/bin/python research/pmxt_replay_audit.py run
.local/pmxt-venv/bin/python research/report_pmxt_replay_audit.py
```

结果和逐笔记录见 [audit v3 报告](../runs/pmxt_reentry_audit_v3/README.md)；机器门槛为 [gate.json](../runs/pmxt_reentry_audit_v3/gate.json)。
v1/v2 是未完成的运行：分别因 binary market ID 序列化、部分市场缺 trace 中断；保留 manifest 与部分诊断，v3 记录替代关系和修复，没有改阈值。改配置或审计代码必须换版本；脚本拒绝覆盖不同版本内容。

## 通过前的研究范围与后续顺序

目前可以报告 archive-observed 的 token 标签覆盖与队列/到达敏感性；不能恢复实际客户端的毫秒 cancel-race 真值，也不能把既有10,656次 legacy回放的结算收益升级成 profitable MM 结论。
小时文件覆盖审计仍为必要条件，缺少 feed 连续性水位意味着它不是充分条件。L2 FIFO 缺失不自动禁止研究，但必须限定为队列假设情景；不能静默把模糊匹配标为真成交/真无成交。

必须先完成下面的门槛，才批量生成 shadow 数据：

1. 独立 source execution 与 received visibility 流接入既有回放器；source arrival 拒单、预留释放、部分成交、pending cancel、回复及实际停止暴露逐事件可追溯；比较 source 同毫秒替代排序。保留 frozen legacy 二进制。
2. 分析 last_trade / L2 数量变化及 complementary matching 的归因边界；两种队列场景共享原始事件、Decimal volume；无法归因标记 unknown，不能把book消失当 fill。
3. 确认历史 tick/min quantity；制定覆盖合格标准与允许的标签年龄。source-only 或较宽标签年龄只能形成新版本敏感性，不能充当客户端可见真值。
4. 冻结统一深度退出、库存/现金/预留、历史费用及风险/资本代价；没有 rebate receipts 时保持0且分别报告运营成本。
5. 审计通过后，100ms 全状态决策网格生成8个独立候选加WAIT，Q0=5、客户端最长1s加cancel生效延迟；严格绑定一致分支库存，不能跨候选相加收益。再冻结时间split、purge、未用test与通过标准。
6. 此后才做 fill/quantity/time 模型、actual-fill 条件价值、尾部风险、direct D 交叉检查；先分桶再浅模型，PM-only/CEX 对照，最后原策略/cooldown/PM-only/PM+CEX和独立shock-cancel对照。

当前的机器门槛按任务书阻断 bulk shadow / model / strategy upgrade。这些后续工作未完成，未伪造校准、容量、净退出收益或新测试结论。

## 历史数字追溯

旧 baseline +152.38、新 baseline +6.16、cancel200 +41.19、joint +34.38 仍是旧回放定义的 legacy 数字，均包含对应终端结算。
对应 [frozen manifest](../runs/pmxt_shock_fixed_tests_20261006/frozen_manifest.json)、[逐笔收益拆分](../runs/pmxt_shock_fixed_tests_20261006/diagnostics/README.md)、[原成交 observer](../runs/pmxt_shock_fixed_tests_20261006/fill_conditional/README.md)。
各市场的 config/summary、逐笔 fills 与 frozen model/exe hash 仍保留；本审计报告另记录所选市场文件 SHA。旧 shock 分桶的 markout 是合成锚、first-after 规则，不能再当作本任务书的严格 token 标签。

# Cross-Venue V0：离线扫描与 PnL 审计

**状态：合成数据、离线确定性验证。没有真实订单、真实成交或已经验证的实盘收益。**

本次选用的组合方向为 Kalshi 与 **Polymarket 国际版 CLOB**；Polymarket US 只作为独立 venue 枚举存在，不能送入国际版适配器。完整实现说明与范围见 [实施报告](../../docs/cross_venue_v0.md)，用户提供的原始规格见 [规格原文](../../docs/cross_venue_v0_spec.zh-CN.md)。

## Python 演示（3.10+，无需第三方依赖）

在仓库根目录执行：

```bash
cd research/cross_venue
python3 -m unittest discover -s tests -v
python3 src/scan.py --input fixtures/synthetic_books.json --out out/opportunities.csv
python3 src/pnl_audit.py --input fixtures/synthetic_fills.json --out out/pnl_audit.csv
```

`scan.py` 只产生带 `SIMULATED` 标签的盘口候选；`pnl_audit.py` 独立读取模拟成交，计算条件最差结算 PnL。两者没有订单接口。

| 合成场景 | 最差结算 PnL |
|---|---:|
| `matched_100` | +1.51 |
| `partial_hedge_60` | -18.60 |
| `hedge_slippage` | -2.49 |
| `signal_no_fills` | 0.00 |

单位是 fixture 定义的 `SYNTHETIC_USD`，即人为设定的共同一元赔付测试单位。CSV 保留规格中的 `_usd` 列名，不能据此认定真实场所币种等价或存在真实美元收益。

所有 `verified_live_realized_pnl_usd` 保持空值。`illustrative_settled_pnl_usd` 使用人为设置的结算方向，只用于复算。输入 SHA-256 只证明本地文件完整性，不认证交易所来源。

`generate_synthetic_fixtures.py` 可重新生成 fixtures；这不会下载数据或批准任何真实规则。

## C++20 集成演示

从仓库根目录执行：

```bash
cmake -S . -B build
cmake --build build --target run_cross_venue_shadow cross_venue_v0_tests -j4
ctest --test-dir build -R '^CrossVenueV0_' --output-on-failure

# 输出目录必须是新目录或空目录，避免覆盖先前的审计包。
build/run_cross_venue_shadow \
  --input research/cross_venue/fixtures/synthetic_books.json \
  --policy config/cross_venue_v0.shadow.json \
  --simulated-fills research/cross_venue/fixtures/synthetic_fills.json \
  --out runs/cross_venue_v0_demo
```

省略 `--simulated-fills` 时仅扫描，模拟成交 CSV 只有表头、执行事件文件为空。程序始终记录 `live_orders_sent=0`、`live_inventory=0`、`verified_live_realized_pnl_usd=null`，这些是此离线程序的状态，不是对外部账户的查询结果。

输出包包括：

- `opportunities.csv`：候选 ID、分腿 L2 成本、费用预算、理论优势、风险资格、规则和盘口引用。
- `rejections.jsonl`：数据、深度和风控拒绝原因。
- `pnl_audit.simulated.csv`：逐模拟场景的库存、总成本、费用、双结果赔付、条件最差 PnL 和成交证据。
- `execution_events.simulated.jsonl`：模拟订单 ACK、成交、撤单与配对状态变化。
- `risk_snapshots.simulated.jsonl`：两账户分别占用的资金；无结算对账时保持预留。
- `manifest.json`：模式、币种、规则与输入哈希、候选数量和未验收状态；只在其他输出写入成功后产生。

Python 与 C++ 的默认预算及规则一致，集成测试逐字段比较 CSV，并比较两次 C++ 回放的全部输出字节。C++ `--policy` 支持修改初始实验阈值；Python 参考扫描器固定使用默认阈值，供基准交叉核验。

#!/usr/bin/env python3
"""Summarize the frozen audit pilot; no training, no counterfactual profit claims."""
import json
from pathlib import Path
from pmxt_replay_audit import HORIZONS, OUT, ROOT, SUITE, sha, write


def report():
    config = json.loads((OUT / 'manifest.json').read_text())
    gate = json.loads((OUT / 'gate.json').read_text())
    if sha(ROOT / 'research/pmxt_replay_audit.py') != config['audit_source_sha256']:
        raise ValueError('frozen audit source changed')
    if sha(SUITE / 'frozen_model.json') != config['original_model_sha256']:
        raise ValueError('frozen predictor changed')
    if sha(ROOT / 'build/run_btc15m_lead_lag_replay') != config['original_executable_sha256']:
        raise ValueError('frozen executable changed')
    cases = [json.loads((OUT / p['slug'] / 'summary.json').read_text()) for p in config['pilots']]
    total_q = sum(c['legacy_fill_quantity'] for c in cases)
    valid_q = [sum(c['valid_label_quantity'][i] for c in cases) for i in range(3)]
    source_q, weighted, source_sum = [0, 0, 0], [0., 0., 0.], [0., 0., 0.]
    invalid = [{}, {}, {}]
    for c in cases:
        fills = json.loads((OUT / c['slug'] / 'fills.json').read_text())
        for f in fills:
            quantity = f['legacy_fill']['quantity_lots']
            for i, h in enumerate(HORIZONS):
                a = f['received_label']['horizons'][str(h)]
                b = f['source_clock_sensitivity']['horizons'][str(h)]
                if a['valid']:
                    weighted[i] += quantity * a['E_usd_per_share']
                if b['valid']:
                    source_q[i] += quantity
                    source_sum[i] += quantity * b['E_usd_per_share']
                if not a['valid']:
                    reason = a['invalid_reason']
                    invalid[i][reason] = invalid[i].get(reason, 0) + quantity
    result = {
        'version': config['version'], 'classification': 'development audit fixtures',
        'markets': len(cases), 'orders': sum(c['orders'] for c in cases),
        'legacy_fills': sum(c['legacy_fills'] for c in cases), 'legacy_quantity': total_q,
        'arrival_crossed_orders': sum(c['arrival_status_counts'].get('post_only_rejected', 0) for c in cases),
        'arrival_unknown_orders': sum(c['arrival_status_counts'].get('unknown_arrival_book', 0) +
                                      c['arrival_status_counts'].get('queue_unknown', 0) for c in cases),
        'arrival_queue_changed_orders': sum(c['arrival_vs_submission_queue_changed_orders'] for c in cases),
        'arrival_tie_sensitive_orders': sum(c['arrival_same_ms_sensitivity_orders'] for c in cases),
        'legacy_fills_on_crossed_orders': sum(c['legacy_fills_on_orders_crossing_at_arrival'] for c in cases),
        'strict_valid_quantities': valid_q,
        'strict_valid_quantity_fraction': [q / total_q if total_q else None for q in valid_q],
        'strict_E_cents_per_share': [100 * s / q if q else None for s, q in zip(weighted, valid_q)],
        'strict_invalid_quantity_reasons': invalid,
        'source_clock_sensitivity_valid_quantities': source_q,
        'source_clock_sensitivity_E_cents_per_share': [100 * s / q if q else None for s, q in zip(source_sum, source_q)],
        'source_clock_warning': 'Source-price diagnostic is not a strategy-visible feature or evidence of client timing accuracy.',
        'legacy_rerun_exact_matches': sum(c['legacy_rerun_summary_exact_match'] for c in cases),
        'gate_passed': gate['passed'], 'bulk_shadow_allowed': gate['bulk_shadow_allowed'],
        'new_strategy_PnL': None,
        'report_source_sha256': sha(Path(__file__))}
    write(OUT / 'results.json', result)
    lines = ['# Re-entry 回放审计 v3：不通过', '',
             '完成首阶段代码审计和 5 个开发市场的原始事件试验。原模型、原二进制、旧结果保持冻结。',
             '本目录不是 shadow 训练集；没有新的收益结果。原始订单未在修复后的执行引擎中重新生成，不能据此重算策略收益。', '',
             '## 到达与队列审计', '',
             '| 市场起始 Unix 秒 | legacy 订单 | legacy 成交/股 | 到达跨价 | 到达不确定 | 队列变化 | 同毫秒敏感 | 接收延迟 p50/p95 ms |',
             '|---|---:|---:|---:|---:|---:|---:|---:|']
    for c in cases:
        lag, counts = c['active_receive_lag_ms'], c['arrival_status_counts']
        lines.append(f"| {c['slug'].split('-')[-1]} | {c['orders']} | {c['legacy_fills']}/{c['legacy_fill_quantity']} | "
                     f"{counts.get('post_only_rejected', 0)} | {counts.get('unknown_arrival_book', 0) + counts.get('queue_unknown', 0)} | "
                     f"{c['arrival_vs_submission_queue_changed_orders']} | {c['arrival_same_ms_sensitivity_orders']} | {lag['p50']}/{lag['p95']} |")
    lines += ['', f"合计 {result['orders']} 笔订单、{result['legacy_fills']} 次成交、{total_q} 股。"
              f"到达跨价 {result['arrival_crossed_orders']} 笔；到达队列不同 {result['arrival_queue_changed_orders']} 笔；"
              f"同毫秒前/后排序改变到达检查 {result['arrival_tie_sensitive_orders']} 笔。",
              f"其中 {result['legacy_fills_on_crossed_orders']} 次旧成交归属于本应拒单的订单。跨价检查会改变后续库存与报价，不能简单从旧 PnL 删掉这些成交。", '',
              '## 严格 token 标签', '',
              '冻结上限：报价接收年龄与来源年龄均不超过 100ms，三个期限相同。此为保守研究假设，不是实测网络精度。',
              '起点为成交 source 时间前严格可见的 exporter-received token midpoint；终点为同一接收时间轴 asof 报价。',
              '这不代表我们客户端可见价格。来源时间轴仅列为敏感性，不进入特征。', '',
              '| 期限 | 主标签有效股数/总股数 | 有效比例 | source 敏感性有效股数 |',
              '|---|---:|---:|---:|']
    for i, h in enumerate(HORIZONS):
        lines.append(f"| {h}ms | {valid_q[i]}/{total_q} | {100 * valid_q[i] / total_q:.2f}% | {source_q[i]} |")
    lines += ['', '低覆盖意味着本版严格标签不足以支撑训练，不能扩大报价年龄上限后悄悄沿用本版本。',
              '无效标签保留引用报价、接收/source 年龄和原因，不填零；未完整验证观察过程的未成交订单 D 为 null。', '',
              '## 产物与复现', '',
              '- `manifest.json`：阈值、假设来源、开发数据分类、原模型/二进制校验和。',
              '- `gate.json` / `results.json`：机器可读阻断项和汇总。',
              '- 每市场 `legacy_rerun.json`：冻结二进制逐字段 summary 一致性。',
              '- `legacy_trace.jsonl` / `orders.json`：报价来源与到达检查，不能当作修复后的订单生命周期。',
              '- `fills.json`：每次部分成交的 S/A/E、D、缺失原因、两条时钟与同毫秒敏感性。',
              '- `event_trace.jsonl.gz`：每次成交附近的原始行情事件，raw_id 可定位原 Parquet。', '',
              '代码审计表和后续范围见 [审计说明](../../../docs/pmxt_reentry_audit.md)。', '',
              '```bash', '.local/pmxt-venv/bin/python research/pmxt_replay_audit.py run',
              '.local/pmxt-venv/bin/python research/report_pmxt_replay_audit.py', '```', '',
              '审计未通过：依任务书停止批量 shadow 数据与最小模型实验。下一步先完成带质量标记的 raw-event 生命周期及统一退出验证。']
    (OUT / 'README.md').write_text('\n'.join(lines) + '\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    report()

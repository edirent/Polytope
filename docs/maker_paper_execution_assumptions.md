# Paper maker execution assumptions V0.2.1

All fills below are scenario estimates from a nonexistent own paper order. Public trades are evidence, not actual exchange receipts. Existing PaperMakerFillModel and PaperMakerExecutionAdapter are reused through an immutable managed interface. Each model runs independently with its own canonical paper account; results are never pooled as earned profit.

| Model | Evidence and assumptions |
|---|---|
| Optimistic | Existing BookCross touch-based approximation, capped by displayed opposing top depth shared across resting orders for the event. Upper-bound scenario. |
| Conservative | Public aggressor trade strictly through the resting price, capped by observed trade lots shared across orders. Quotes are not filled simply for touching the BBO. |
| Queue aware | Minimum resting time; displayed same-price queue ahead at submission is depleted by qualifying public trade volume before hypothetical fills. No exact FIFO or own queue evidence is claimed; cancellation-ahead credit is zero. |
| Adverse selection | Existing MidCross adverse-movement fill approximation plus fourfold cancel acknowledgement delay. Stress scenario, not a calibrated probability model. |

The gateway owns scheduled positive submission/cancel latency and post-only state. Market evidence physically received before configured arrival cannot fill the hypothetical order merely because consumer processing was delayed. Current availability controls decisions; post-fill 5-second marks are analysis only. Repeated source event IDs cannot consume liquidity twice. REST/heartbeat/unrelated-asset updates do not generate maker fills.

The four models have intentionally different evidence requirements. Public coverage gaps, incomplete depth, unknown queue priority and unavailable private account data prevent verified execution probability or profitability. Delayed processing after a simulated cancel/expiry uses a conservative current-availability convention, not reconstructed exchange event matching.

Historical real-market fee assumptions are bound to frozen market metadata. Synthetic scenarios explicitly charge a configured per-lot fee; no dynamic fee schedule or rebates are invented. Estimated rewards, confirmed reward receipts, terminal payoff estimates and settled cash remain distinct; no reward/settlement receipt implementation is claimed. Canonical realized PnL uses existing ledger conventions; net equity includes all cash fees and marked inventory. Unusable/stale marks remain labeled estimates.

Metrics include quoted spread/refresh/lifetime, partial/terminal fills, cancel-fill races, fill ratio by declared basis, inventory, fees, net equity, markout and drawdown. Finite real Polymarket PAPER/SHADOW collection is reported separately from historical replay. The V0.2.0 Kalshi authentication evidence is retained but is not re-described as newly authenticated V0.2.1 data.

## Completed validation evidence

Final 599.796-second real-recording scenarios: optimistic/conservative/queue-aware had no fills and zero simulated net equity PnL; adverse-selection stress had two fills and lost 150,000 ticks (0.15 USDC). Constructed synthetic outcomes are separately labeled in the model CSV/report. Final balanced live PAPER/SHADOW conservative fills were zero. Five-second markouts require an available admitted public mark between five and six seconds after the hypothetical fill; otherwise the horizon is unavailable. No quoted spread or paper receipt is exchange-realized profit.

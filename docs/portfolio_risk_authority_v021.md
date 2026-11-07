# Portfolio risk authority V0.2.1

The existing RiskEngine is the admission authority and its existing ReservationBook owns both legacy bundle reservations and managed maker commitments. PortfolioAccountView is a read-only value projection from canonical PaperLedger, not another balance store.

Before approval the single writer checks actual proposed quantities/prices/fees against funded paper cash minus all outstanding cash commitments and an explicit safety buffer; existing inventory minus all outstanding sells; aggregate inventory plus open buys; global/event/venue/account limits; per-instance strategy budgets and order limits. Bundle decision quantities are bound back into the existing reservation, replacing unit-ratio quantities before any adapter operation. Unknown and cancel-pending commitments do not expire on local timers.

| Event | Cash commitment | Sell inventory commitment |
|---|---|---|
| Submit / unknown acknowledgement | Full unfilled cost plus fee allowance | Full unfilled lots |
| Partial fill | Canonical cash changes once; only remaining cost stays reserved | Filled lots leave canonical inventory; remaining lots stay reserved |
| Cancel requested / timeout / reject | Retained | Retained |
| Confirmed simulated terminal event | Remaining commitment released | Remaining commitment released |
| Late authoritative simulated fill after terminal | Posted if valid, incident/freeze | Never silently ignored; ledger failure blocks recovery |

Global exposure includes canonical inventory cost plus unfilled buys. Existing sells cannot offset inventory before a confirmed fill. Related YES/NO assets aggregate by exact venue and condition identity, with positive costs summed. No title-based event equivalence or cross-venue offset is permitted. The runtime deliberately accepts only the explicitly configured PAPER/USDC Polymarket account; multi-wallet collateral/haircuts remain future work.

Risk checks and commits are serialized. Deterministic priority then instance ID defines allocation, with each instance's configured budget. The policy is disclosed in telemetry. Strategy attribution tracks receipt-derived ownership for budgets and PnL, while all tradable holdings remain canonical. Shared inventory sales transfer attributed cost between owners as a projection; they never transfer actual cash between competing ledgers.

Invariant checks reconcile open order remaining quantities to active reservations, sell commitments to canonical inventory, and reserved cash to funded balances. Identity conflicts, unexplained late fills, mismatches, limit breaches, unknown ACK or journal failures freeze risk increases. Strategy stop and global stop cannot manufacture cancel/settlement confirmation.

Fees are explicit per-lot paper assumptions. The selected historical real market metadata has feesEnabled=false; this is not certification of a future fee schedule. Unsettled proceeds, rewards and payoff estimates do not become available cash without a supported receipt. Private exchange reconciliation is UNCONFIGURED; verified_live_realized_pnl is null.

## Completed validation evidence

Final independent audit matched cash, fee postings, inventory/cost, rule/config/input identities, per-strategy attribution and outstanding commitments in all ten final runs. Real-recorded scenarios peaked at 4.90% of funded cash in reservations; balanced live PAPER peaked at 3.97%. These are observed paper capacities, not exchange-verified balances or deployable limits.

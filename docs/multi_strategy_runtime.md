# Multi-strategy native runtime

Schema 3 selects strategy=multi_strategy and explicit instruments, strategies, maker_execution and finite existing runtime limits. Schema 1/2 retain previous recorded/live semantics. REPLAY, PAPER and SHADOW share the same plugin, decision, risk and OMS interfaces; LIVE is rejected before activation.

One single-writer Session owns ordering. It updates authoritative state, advances previously submitted simulated orders, projects canonical inventory, dispatches immutable value frames in ascending configured priority/instance ID, collects typed requests, applies shared admission, persists plans, invokes the selected paper adapter, posts durable receipts and publishes derived read models. Borrowed combinatorial evidence is consumed synchronously before the next scan; no plugin retains book references.

Each instance records strategy/instance ID, positive configuration epoch, frozen model hash, subscriptions, budget and callback budget. Original request payloads and metadata remain distinct from executable plans. Generic MarketMakingEngine parameters are supported; unsupported model fields fail startup. Specialized BTC product-provider orchestration is retained in research but is not silently claimed as a verified plugin here.

Plugin exceptions discard the callback's collected requests, quarantine that instance and request separately authorized paper cancellation for its known orders. Other strategies remain independent. A returned callback over its processing budget is quarantined; this is cooperative containment, not preemption of a permanently blocked C++ callback. Wall-time budget breaches can differ under a changed host workload; replay certification must inspect those incidents rather than assume they reproduce. A hard watchdog/control-isolation contract remains unverified.

Global risk freeze stops all risk increases without releasing commitments. Strategy-local stop suspends one instance and preserves the others. Account/OMS/reservation inconsistency and journal failures stop or block recovery; no automatic restart clears unknown exposure.

Operations use the existing paper_backend and dashboard ?view=runtime. New read-only endpoints include /api/v1/strategies, /api/v1/maker-orders and /api/v1/portfolio-reservations. Desired requests, actual orders, global resources, per-instance lifecycle and paper PnL are separate panels. POST remains rejected. Financial values are computed natively, not in the browser.

Run the executable with a fresh output directory and frozen profile. SIGTERM drains the active transaction. An open/cancel-pending paper order remains encumbered at bounded EOF unless a simulated terminal acknowledgement was observed. --recover verifies executable/config/artifact/input identity, reconstructs committed prefixes, restores known tail receipts and blocks unknown tails. No host-reboot/software-upgrade migration is certified.

The evidence directory contains exact commands, profiles, hashes and results. Longer live runs, new private credentials and cross-venue execution are not implicitly enabled by this release.

## Completed validation evidence

Final RC3 live follow-up used market 682705, public Polymarket feeds only. PAPER observed 180.135053465 seconds, 5,815 inputs and 74 simulated OMS orders. SHADOW observed 180.073589824 seconds and 5,768 inputs with no adapter invocation/fill. Both conservative fill totals were zero and controlled reconnection was observed. Sampled admission/degradation remains explicit in live_health_audit.json. No new Kalshi authentication, continuous L2 completeness or long soak is claimed.

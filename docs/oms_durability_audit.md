# OMS durability audit — V0.2.0

Date: 2026-11-06 (America/Chicago). Scope: the current native runtime and existing ExecutionGateway. LIVE remains unsupported.

## Finding and change

The first slice persisted RISK_RESERVED, then SUBMIT_STARTED, and only persisted the full plan through a gateway observer. The observer was before the actual adapter call, but a crash after SUBMIT_STARTED could leave no durable client order IDs or full parameters. Preparation and invocation were not explicit enough to audit independently.

The existing gateway now exposes `prepare(envelope, context)` and `submit_prepared(plan, envelope, context)`. Preparation calls the existing planner and validator only. Submission rebuilds the authorized expected plan and compares every order/economic identity field before entering the existing execution lifecycle. No second OMS was introduced. Legacy approved submission interfaces remain for existing callers and tests.

The runtime persists:

```text
STRATEGY_INTENT + ORDER_DECISION
RISK_DECISION + RISK_RESERVED
PLAN_PREPARED: full children, client IDs, sides, quantities, limits,
              price protection, plan times, cost/slippage/payout limits,
              decision/policy/proof hashes and reservation authorization
SUBMIT_STARTED: bound plan ID and plan identity hash
ADAPTER_INVOKED: synchronously durable, SIMULATED_PAPER_ONLY
[existing PaperExecutionAdapter]
EXECUTION_REPORT: stable simulated receipt ID, delta quantities, price, fees
[existing PaperLedger posting]
RESERVATION_DISPOSITION + OMS_RESULT
INPUT_COMMIT
```

The gateway's pre-adapter hook rejects invocation unless the prepared identity matches, the durable submission authorization is present, and both the financial journal and its raw-input dependency have no unflushed records. An observer notification is not an exchange acknowledgment. Prepared plans are not executed orders. SHADOW stops after durable would-submit auditing and releases its paper-only reservation without calling the adapter.

## Durability classes and measured I/O

Initial real public-feed diagnostics measured approximately 9 ms median fsync on this WSL host and approximately 38.5 ms median per-input processing with synchronous writes for every market/audit record. Queue accumulation was observed; those runs were deliberately stopped after roughly 253 seconds, rather than reported as 30-minute successes.

V0.2.0 separates durability classes within the same append-only journals:

| Record class | Policy | Consistency boundary |
|---|---|---|
| High-volume raw market input, rejected-signal audit, state audit, INPUT_BEGIN/COMMIT without an economic action | Written in single-writer order; target checkpoint every 250 ms | Raw-input journal flushed before financial/audit journal at each checkpoint. The newest data-only tail can require review after a crash. |
| Positive order intent/decision, approved risk/reservation, full prepared plan, submit marker, adapter invocation, reports, reservation disposition, stop/mismatch/error | Synchronous durable append | Flush the raw dependency first, then fsync the financial event, before proceeding. |
| Accounting | Apply only after the receipt has been durably appended | Receipt survives a crash between journal and ledger application. |
| Dashboard/history | Derived telemetry | Atomic snapshot publication; not an accounting authority. Its blocking I/O cost is measured separately. |

Buffered market auditing does not permit buffered submission or buffered accounting receipts. The journal exposes its current head and durable head separately. The 250 ms checkpoint is a target, not a hard real-time guarantee when storage stalls. A torn tail is rejected rather than silently truncated. Disk errors fail closed.

## Recovery contracts

| Crash point | Recovery result |
|---|---|
| Before PLAN_PREPARED or after prepared plan but before SUBMIT_STARTED | No adapter was permitted to run. Tail is classified NOT_SUBMITTED, no phantom order; economic encumbrance is zero, while the incomplete transaction remains reviewable/blocked. |
| After durable SUBMIT_STARTED, before/within adapter call | ACK_UNKNOWN_QUERY_BEFORE_RETRY; retain full plan, authorization and conservative pending encumbrance. Never blindly resend. |
| After a durable simulated fill receipt, before ledger/commit | Restore the known receipt through the existing ledger exactly once; retain unresolved tail and block new exposure. |
| Complete committed prefix | Verify event and account digests while replaying with the identical executable/artifact/config. No duplicate financial events are appended. |
| Corrupted/torn journal | Refuse activation without modifying the evidence. |

There is no authenticated private-account query implementation in this release. Query-before-retry means an unknown outcome is frozen until authoritative reconciliation is available; it does not mean a query has secretly succeeded. A local timeout, PID exit, TTL or kill switch is not proof of external cancellation or settlement.

## Accounting and identifiers

ExecutionReport now accepts a stable `fill_id` and separate `fee_tick`. Runtime receipts are explicitly `SIMULATED:<plan>:<child>:<event time>`; public market trades never enter this receipt path. PaperLedger deduplicates receipt delivery and rejects conflicting economic fields for an existing ID. Legacy tuple-derived IDs remain for existing callers. FillTracker uses supplied IDs when present and retains its legacy fallback otherwise.

Cash, positions and fees remain owned by the existing PaperLedger/PositionLedger. PaperPortfolio remains a derived valuation projection. Conditional terminal payout/PnL does not credit cash or establish settlement. The private reconciliation interface is read-only and UNCONFIGURED; `exchange_account_reconciled=false` and `verified_live_realized_pnl=null` remain explicit.

## Tests and limits

Executable faults exercise preparation/submission/receipt crashes, repeated intent/market/report delivery, ENOSPC, partial journal writes, strategy exceptions, ledger mismatch, stop/restart, stale input, market closure, Kalshi sequence gaps and independent slow operations clients. Native tests cover bounded queue FIFO/capacity, partial fills, reservation retention, cancellation authority, separate fees and conflicting receipt IDs. Exact counts/logs are in the release evidence summary.

This runtime constructs only PaperExecutionAdapter. Legacy gateway internals still contain pre-ack preparation and unmanaged submission paths for other callers; they are not authoritative exchange truth and are not enabled for live money here. Asynchronous maker orders, pending-cancel resources, authoritative exchange fill IDs and authenticated query/reconciliation require another integration milestone. One atomic paper plan is admitted at a time; these contracts must not be generalized to concurrent live orders by configuration alone.

## Completed evidence

The final frozen executable passed all 42 operational scenarios, including the three durable crash boundaries, no-phantom-order recovery, unknown-ACK encumbrance, repeated report delivery, ledger mismatch, ENOSPC, a real partial journal write and unchanged corrupt-tail rejection. See `runs/runtime_v020/evidence_20261106/operational_faults.log` and per-scenario `fault_evidence/`. Native lifecycle tests passed 21 assertions. The full regression suite retains the pre-existing maker failure, rather than claiming a clean suite.

Both real 30-minute runs had zero approvals, submissions and receipts; they validate recording, rejected decisions and no-economic-change replay, not real-market execution latency. A separate 200-input synthetic profitable-book benchmark exercised 199 approved plans through durable preparation and the Paper adapter. Measured p50/p95/p99 in ms: strategy-to-risk 10.989/16.302/20.453; risk-to-durable-plan 16.411/24.876/28.949; durable-plan-to-adapter 11.132/17.021/21.110. These include synchronous journal work on this host and have insufficient samples for p99.9. They are simulated order-path evidence, not external acknowledgement or trading results.

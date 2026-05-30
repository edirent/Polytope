# Execution Gateway

## 1. Executive Summary

The Execution Gateway is the paper execution boundary after Risk approval.

The central boundary is:

```text
ApprovedIntent + source OpportunityIntent + latest MarketStateSnapshot
  -> ExecutionGateway
  -> OrderPlan + ExecutionReport + ReservationDisposition
```

Execution consumes an `ApprovedIntentEnvelope`, which contains the Risk-approved
intent plus the source `OpportunityIntent`. Execution produces `OrderPlan`
records, child order reports, and reservation dispositions back to Risk.

Execution v0 uses `PaperExecutionAdapter` by default. `LiveExecutionAdapter` is
disabled by default and does not access the network when live execution is not
explicitly enabled.

`ApprovedIntent` is not an order. `OrderPlan` is not necessarily submitted to a
live venue. v0 has no wallet, private key, signer, or live order output.

## 2. Scope / Non-Scope

In scope:

- Validate a Risk-approved intent envelope.
- Convert approved BUY legs into deterministic `OrderPlan` / `ChildOrder`
  records.
- Validate plan structure, reservation presence, child order count, TTL, and
  duplicate client order IDs.
- Simulate paper fills from `MarketStateSnapshot` depth.
- Support paper atomic all-or-fail mode.
- Support paper sequential mode for partial-fill state-machine tests.
- Track order and plan state transitions.
- Track fills, filled quantity, remaining quantity, average fill price, and
  plan-level fill state.
- Mark multi-leg partial fills as `HedgeRequired`.
- Publish `ExecutionReport`.
- Publish `ReservationDisposition` so Risk can consume, release, expire, or
  mark hedge-required reservations.
- Emit execution counters and latency metrics.
- Provide workflow verification through `verify_execution_workflow`.

Non-scope:

- No risk approval.
- No signal construction.
- No Oracle access.
- No wallet signing.
- No private key handling.
- No REST order placement.
- No live venue order submission in v0.
- No direct mutation of `RiskLedger`.
- No direct feed/decode/state mutation.

Execution is an adapter and lifecycle layer. It cannot approve risk and cannot
create trading ideas.

## 3. Inputs

### ApprovedIntentEnvelope

Execution consumes only `ApprovedIntentEnvelope` as its approval input.

The envelope must contain:

- source `OpportunityIntent`
- risk approval / decision identity
- nonzero `reservation_id`
- matching `bundle_id`
- matching `idempotency_key`
- positive approved bundle quantity

Missing reservations are rejected. Execution never bypasses Risk.

### MarketStateSnapshot

`PaperExecutionAdapter` uses snapshots to simulate fills.

BUY child orders consume asks from lowest price to highest price, respecting
limit price and available depth. SELL live/proceeds semantics are not enabled in
Execution v0 unless explicitly added later.

## 4. Outputs

### OrderPlan

`OrderPlan` is the deterministic plan generated from an approved intent. It
contains:

- `plan_id`
- source intent and approval IDs
- reservation ID
- bundle ID
- child orders
- cost / edge limits
- creation and expiry timestamps
- idempotency key

`OrderPlan` is not necessarily submitted to a live venue. In v0 it is submitted
to the paper adapter.

### ChildOrder

Each BUY intent leg becomes a `ChildOrder` with:

- deterministic order ID
- deterministic client order ID
- market / asset identity
- quantity
- limit price
- expected VWAP / worst allowed price

### ExecutionReport

Reports describe child order lifecycle results:

- filled
- partially filled
- cancelled
- failed / rejected
- expired

Terminal plan outcomes publish reports through `ExecutionReportPublisher`.

### ReservationDisposition

Execution must emit reservation disposition events back to Risk:

```text
Filled       -> Consume
Failed       -> Release
Cancelled    -> Release
Expired      -> Expire or Release, depending on failure point
HedgeRequired -> hedge-required disposition/counter
```

Execution does not directly mutate `RiskLedger`. Risk owns reservation state.

## 5. Pipeline

Submit flow:

```text
ApprovedIntentEnvelope
  -> ExecutionPlanner.build_plan
  -> PlanValidator.validate
  -> ExecutionPlanStore
  -> IExecutionAdapter.submit_plan
  -> OrderStateMachine / PlanStateMachine
  -> FillTracker
  -> PartialFillPolicy
  -> ExecutionReportPublisher
  -> ReservationDispositionPublisher
```

Any invalid envelope or invalid plan stops before adapter submission.

## 6. Paper Execution

### PaperAtomic

Paper atomic mode is all-or-fail.

The adapter first checks every child order against current snapshot depth and
limit price. If all legs can fully fill, the whole plan is marked filled. If any
leg cannot fully fill, the whole plan fails and no partial fill is accepted.

### PaperSequential

Paper sequential mode simulates child orders in order. It can produce partial
fills when configured to allow them. This mode exists to test lifecycle,
partial-fill, cancel, and hedge-required behavior.

For multi-leg plans:

```text
some leg filled and not all legs filled -> HedgeRequired
```

## 7. Live Adapter Boundary

`LiveExecutionAdapter` is disabled by default.

With `EXECUTION_ENABLE_LIVE=OFF`, live submit and cancel return disabled results,
polling returns no reports, and no network call is made.

Execution Gateway v0 has no private key, no wallet, no signer, and no live venue
order output.

## 8. State Machines

Child order lifecycle:

```text
Created -> Planned -> Sent -> Acked
Acked -> Filled
Acked -> PartiallyFilled -> Filled
PartiallyFilled -> CancelRequested -> Cancelled
Any non-terminal -> Failed / Expired
```

Plan lifecycle:

```text
Created -> Planned -> Sent -> Acked
Acked -> Filled
Acked -> PartiallyFilled -> HedgeRequired
Acked -> Failed / Expired
CancelRequested -> Cancelled
```

Terminal states cannot transition further.

## 9. Metrics

Execution metrics include:

```text
execution.plan.created
execution.plan.submitted
execution.plan.filled
execution.plan.failed
execution.plan.expired
execution.plan.hedge_required

execution.child.created
execution.child.filled
execution.child.partial
execution.child.cancelled
execution.child.failed

execution.report.published

execution.reservation.consume
execution.reservation.release
execution.reservation.expire
execution.reservation.hedge_required

execution.latency.submit_ns
execution.latency.fill_simulation_ns
```

`verify_execution_workflow` prints these metrics as workflow counters. Latency
values are reported but not included in deterministic output hashes.

## 10. Workflow Verification

Command:

```bash
build/verify_execution_workflow \
  --approved-intents tests/fixtures/execution/approved_intents_positive.jsonl \
  --source-intents tests/fixtures/execution/opportunity_intents_positive.jsonl \
  --snapshots tests/fixtures/execution/market_state_snapshots_execution.json \
  --config tests/fixtures/execution/execution_config_paper.json \
  --check-determinism
```

The verifier reports:

- approved intents loaded
- plans and child orders created
- adapter mode and plan outcomes
- fill counts and total filled cost
- execution reports
- reservation consume / release / expire / hedge-required counts
- execution metrics
- deterministic output hash

Fixture scenarios cover:

- positive paper full fill
- insufficient depth
- expired approved intent
- duplicate idempotency key
- sequential paper partial fill
- paper cancel path
- disabled live adapter

## 11. Failure Conditions

Workflow verification fails if:

- missing reservation reaches adapter submission
- invalid plan passes validation
- paper atomic produces partial fill
- insufficient depth consumes reservation
- multi-leg partial fill is not marked `HedgeRequired`
- disabled live adapter attempts network behavior
- output hash is nondeterministic
- execution emits live order output in v0

## 12. Runtime Boundary

Execution receives Risk-approved input and returns execution lifecycle output.

It does not approve risk, mutate the Risk ledger directly, construct signals,
call Oracle, read Feed, decode packets, or own market state.

The only feedback channel to Risk is `ReservationDisposition`.

## 13. Current Limitations

- BUY-only paper execution is supported.
- SELL pricing/proceeds semantics are not enabled.
- Live execution is a disabled stub.
- Paper fill simulation uses snapshot depth and does not model queue priority.
- Hedge handling is reported as state, not resolved by a live hedge engine.
- Reservation disposition is emitted for Risk to consume; Execution does not
  own reservation lifecycle persistence.

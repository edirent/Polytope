# Risk Engine

## 1. Executive Summary

The Risk Engine is the approval and reservation layer between paper signals and
future execution.

The central boundary is:

```text
OpportunityIntent + latest MarketStateSnapshot + RiskPolicySnapshot
  -> RiskEngine
  -> RiskDecision / ApprovedIntent
```

Risk consumes `OpportunityIntent` records produced by the Signal Engine. Risk
outputs either a rejected `RiskDecision` or an approved `ApprovedIntent`.

`ApprovedIntent` is not an order. It is a risk-approved intent with a
reservation. Execution must still own order construction, order routing, order
submission, cancel/replace behavior, and exchange/API interaction.

The Risk Engine does not execute.

## 2. Scope / Non-Scope

In scope:

- Validate `OpportunityIntent` structure and lifecycle fields.
- Verify signal evidence fields such as artifact hash, bundle hash, snapshot
  hash, expiry, and idempotency key.
- Enforce kill switch, expiry, duplicate, and approval-rate guards.
- Check latest market-state usability before approval.
- Recompute cost from the latest `MarketStateSnapshot`.
- Reject stale books, bad market state, insufficient depth, excessive cost
  drift, low post-risk edge, max loss, exposure, inventory, and partial-fill
  risk.
- Track pending reservations in exposure and inventory checks.
- Create a reservation before approval.
- Publish auditable `RiskDecision` / `ApprovedIntent` records.
- Emit risk counters and evaluate-latency metrics.
- Provide workflow verification through `verify_risk_workflow`.

Non-scope:

- No order construction.
- No order execution.
- No REST order placement.
- No wallet signing.
- No private key handling.
- No exchange API calls.
- No signal generation.
- No Oracle compilation.
- No raw feed decode.
- No market-state mutation.

Risk is a gate. It can approve or reject an intent, but it cannot turn that
intent into an executable order.

## 3. Inputs

### OpportunityIntent

`OpportunityIntent` is the only signal-side input.

Required fields include:

- `intent_id`
- `bundle_id`
- `status = PaperOpportunity`
- `bundle_qty`
- `total_edge_tick`
- `created_ts_ns`
- `expires_at_ns`
- `idempotency_key`
- `oracle_artifact_hash`
- `bundle_hash`
- `snapshot_version_hash`
- at least one leg

Incomplete intents are rejected before any VWAP or exposure work is performed.

### MarketStateSnapshot

Risk uses the latest available snapshots, not only the snapshot evidence
embedded in the signal intent.

Risk checks:

- snapshot exists
- snapshot is live
- snapshot is usable for depth
- not recovering
- not crossed
- not closed
- not resolved
- book age is within policy

If the latest snapshot hash differs from the signal snapshot hash, Risk does not
blindly reject every mismatch. A fresh mismatch can require repricing. Risk then
recomputes cost from the latest book and applies cost-drift policy.

### RiskPolicySnapshot

`RiskPolicySnapshot` is the immutable policy input for one evaluation.

It contains:

- kill switch
- minimum post-risk edge thresholds
- max cost / exposure / inventory limits
- max book age
- max intent age
- max cost drift
- max slippage
- partial-fill depth margin
- approval-rate limits

The policy hash is deterministic and included in approved decisions.

## 4. Outputs

### RiskDecision

`RiskDecision` is emitted for every evaluated intent.

It records:

- approved or rejected status
- reject reason
- policy version
- policy hash
- reject detail

Reject reasons are explicit so downstream logs can distinguish invalid input,
expired intent, duplicate intent, kill switch, stale book, insufficient depth,
cost drift, low edge, exposure, inventory, partial-fill risk, max loss, and rate
limit failures.

### ApprovedIntent

`ApprovedIntent` is emitted only after all guards pass and a reservation is
created.

An approval requires:

```text
all guards pass
  -> ReservationBook.try_reserve succeeds
  -> ApprovedIntent has reservation_id
```

Reservation is required for approval.

`ApprovedIntent` is still not an order. It is the object future Execution may
consume as input to build an order request.

## 5. Pipeline

Current Risk pipeline order:

```text
KillSwitchGuard
IntentValidator
IntentEvidenceVerifier
IntentExpiryGuard
DuplicateIntentGuard
RateLimitGuard
MarketStateGuard
SnapshotFreshnessGuard
CostRevalidator
EdgeGuard
MaxLossGuard
ExposureGuard
InventoryGuard
PartialFillGuard
ReservationBook.try_reserve
Approve
```

Any rejection stops the pipeline. Rejected intents do not reserve exposure.

Only a successful reservation can produce an approved decision.

## 6. Cost Revalidation

Risk recomputes cost from the latest snapshot.

Signal cost is evidence, not authority.

Repricing path:

```text
OpportunityIntent legs
  -> latest MarketStateSnapshot depth
  -> VWAPRevalidator
  -> CostRevalidationResult
  -> EdgeGuard / MaxLossGuard / ExposureGuard
```

BUY legs consume asks. SELL leg repricing is unsupported in Risk v0 unless it is
explicitly enabled later with a signed cost/proceeds convention.

Risk computes:

- `risk_total_cost_tick`
- `risk_bundle_qty`
- fee
- slippage buffer
- latency buffer
- cost drift versus signal estimate

If cost drift exceeds policy, the intent is rejected.

## 7. Exposure And Reservations

Risk checks exposure including pending reservations.

This is a hard invariant:

```text
pending approved-but-not-executed reservations count toward exposure
```

The ledger tracks:

- total reserved cost
- total reserved exposure
- reserved lots per asset
- reserved market exposure
- active / released / expired / consumed reservation counts

The exposure guard checks:

- total exposure after approval
- per-market exposure after approval

The inventory guard checks:

- post-approval reserved lots per asset

This prevents multiple approved intents from over-allocating the same risk
budget before Execution reports final order or fill status.

## 8. Partial Fill Risk

Risk v0 implements a conservative partial-fill guard.

For a single BUY leg, there is no cross-leg hedge breakage in Risk v0.

For multi-leg intents, Risk requires available depth to exceed requested depth
by the configured margin:

```text
available_depth >= requested_qty * min_depth_margin_ratio
```

An unhedged worst-case loss interface exists as a future extension, but v0 only
enforces the depth-margin layer.

## 9. Audit Trace

Every evaluation produces an audit trace.

The trace records each executed guard:

- guard name
- pass / fail
- rejection type
- reason

The trace stops at the rejecting guard. This makes a risk decision explainable
without reconstructing the entire pipeline from logs.

JSONL risk decision output must not contain order fields.

## 10. Metrics

Risk metrics are lightweight counters plus latency placeholders.

Current counters:

```text
risk.evaluate.count
risk.approve.count
risk.reject.count

risk.reject.invalid_intent
risk.reject.expired
risk.reject.duplicate
risk.reject.kill_switch
risk.reject.stale_book
risk.reject.insufficient_depth
risk.reject.cost_drift
risk.reject.low_edge
risk.reject.exposure
risk.reject.inventory
risk.reject.partial_fill
risk.reject.max_loss
risk.reject.rate_limited

risk.reservation.created
risk.reservation.expired
risk.reservation.released

risk.vwap.recomputed
risk.latency.evaluate_ns
```

`verify_risk_workflow` prints these metrics in its summary.

## 11. Module Structure

Current module layout:

```text
engine/risk/
├── public/
│   ├── RiskTypes.h
│   ├── RiskConfig.h
│   ├── RiskPolicySnapshot.h
│   ├── RiskDecision.h
│   ├── ApprovedIntent.h
│   ├── RiskResult.h
│   └── RiskAuditTrace.h
├── validate/
│   ├── IntentValidator
│   └── IntentEvidenceVerifier
├── ledger/
│   ├── RiskLedger
│   ├── ExposureLedger
│   ├── InventoryLedger
│   ├── ReservationBook
│   └── Reservation
├── guards/
│   ├── KillSwitchGuard
│   ├── IntentExpiryGuard
│   ├── DuplicateIntentGuard
│   ├── RateLimitGuard
│   ├── MarketStateGuard
│   ├── SnapshotFreshnessGuard
│   ├── EdgeGuard
│   ├── MaxLossGuard
│   ├── ExposureGuard
│   ├── InventoryGuard
│   └── PartialFillGuard
├── reprice/
│   ├── CostRevalidator
│   ├── VWAPRevalidator
│   ├── FeeRevalidator
│   ├── SlippageRevalidator
│   └── LatencyRevalidator
├── publish/
│   ├── RiskDecisionPublisher
│   ├── CapturingRiskPublisher
│   ├── JsonlRiskDecisionWriter
│   └── ApprovedIntentPublisher
├── metrics/
│   └── RiskMetrics
├── core/
│   ├── RiskEngine
│   ├── RiskContext
│   ├── RiskPipeline
│   └── RiskWorkflow
└── tools/
    └── verify_risk_workflow
```

Dependency direction:

```text
engine_risk
  -> engine_signal public OpportunityIntent
  -> engine_state MarketStateSnapshot
```

Forbidden dependencies:

```text
engine_risk -> engine_execution
engine_risk -> wallet/signing code
engine_risk -> exchange REST client
engine_risk -> engine_feed internals
engine_risk -> engine_decode internals
engine_risk -> engine_chain_confirm internals
```

## 12. Workflow Verification

Workflow tool:

```text
build/verify_risk_workflow \
  --intent-fixture tests/fixtures/risk/opportunity_intents_positive.jsonl \
  --snapshot-fixture tests/fixtures/risk/market_state_snapshots_risk.json \
  --risk-config tests/fixtures/risk/risk_config_small.json \
  --check-determinism
```

Summary sections:

```text
risk_workflow
decisions
repricing
reservation
metrics
hashes
```

Expected baseline behavior:

- positive fixture produces at least one approval
- expired fixture rejects expired intent
- duplicate fixture rejects duplicate idempotency key
- stale snapshot fixture rejects stale book
- tight exposure fixture rejects exposure limit
- partial fill fixture rejects partial-fill risk
- determinism check passes

## 13. Failure Conditions

Risk workflow failure conditions include:

- invalid intent is approved
- expired intent is approved
- duplicate idempotency key is approved twice
- stale book is approved without repricing and policy check
- insufficient depth is approved
- cost drift exceeds policy but intent is approved
- low post-risk edge is approved
- max loss exceeds policy but intent is approved
- exposure or inventory ignores pending reservations
- partial-fill risk is ignored for multi-leg intents
- approval occurs without reservation
- Risk emits an executable order

Any of these violates the Risk Engine boundary.

## 14. Runtime Boundary

Runtime ownership is:

```text
Signal Engine
  -> creates OpportunityIntent

Risk Engine
  -> validates intent
  -> recomputes cost
  -> checks policy
  -> reserves exposure
  -> emits RiskDecision / ApprovedIntent

Execution Engine
  -> constructs orders
  -> signs / submits / cancels / replaces orders
```

Execution must still own order construction.

Risk must never submit an order directly.

## 15. Current Limitations

- SELL repricing is unsupported by default.
- Partial-fill risk v0 uses depth margin only.
- There is no live execution adapter.
- Reservation consume/release will need integration with future Execution state.
- Metrics are in-process counters and latency placeholders, not a production
  telemetry backend.
- Policy loading is fixture/tool oriented in v0.

## 16. Next Layer

The next layer after Risk is Execution.

Execution must consume only approved intents, construct concrete orders, submit
them to the venue, and report reservation release/consume events back to Risk.

Risk approval is necessary but not sufficient for execution. Execution still
owns the final order representation and venue-side behavior.

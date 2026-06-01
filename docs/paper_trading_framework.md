# Paper Trading Framework

## Executive Summary

Paper Trading is an observability and simulation layer. It consumes events that
are already produced by Signal, Risk, and Execution, then builds read models for
paper account state, PnL, performance, regimes, and dashboard snapshots.

Paper Trading is not live trading. It does not approve risk, construct orders,
submit orders, own wallets, or mutate execution state.

## Scope

Paper Trading owns:

- paper cash, fill, and position ledgers
- paper PnL from execution reports
- performance metrics
- regime summaries
- dashboard read models
- read-only backend routes
- read-only frontend dashboard

Paper Trading does not own:

- signal generation
- risk approval
- order planning
- execution submission
- live exchange adapters
- wallets, private keys, or signers
- runtime configuration mutation

## Data Flow

The intended flow is:

```text
OpportunityIntent
RiskDecision / ApprovedIntent
OrderPlan
ExecutionReport
ReservationDisposition
MarketStateSnapshot mark update
  -> PaperTradingEngine
  -> PaperLedger / PaperPortfolio / PaperPnLEngine
  -> PerformanceMetricsEngine / RegimeEngine
  -> DashboardReadStore
  -> ReadOnlyGateway
  -> Frontend Dashboard
```

Only `ExecutionReport` fill data can change paper cash, positions, and PnL.
`OpportunityIntent`, `RiskDecision`, and `ApprovedIntent` are observations only.

## Runtime Boundary

`PaperTradingEngine` only consumes outputs from other layers. It must not call
Signal, Risk, or Execution to create new activity. It must not feed back into
RiskLedger or ExecutionGateway except through future explicit reporting channels.

Paper account state is local simulation state. It is not exchange account state.

## PnL Rules

Paper PnL only comes from `ExecutionReport` / fill events.

Rules:

- duplicate execution reports are ignored idempotently
- unsupported SELL fills are rejected in v0
- mark-to-mid and liquidation PnL are tracked separately
- missing or degraded market marks degrade PnL quality
- opportunities and risk decisions never create PnL

This prevents dashboards from treating proposed opportunities as realized or
unrealized profit.

## Dashboard Read Model

`DashboardReadStore` is a copy-out read model for observers. It is a dashboard
surface, not an operational control plane.

The frontend reads from:

- `GET /api/v1/health`
- `GET /api/v1/snapshot/latest`
- `GET /api/v1/markets`
- `GET /api/v1/intents?limit=100`
- `GET /api/v1/risk-decisions?limit=100`
- `GET /api/v1/execution-reports?limit=100`
- `GET /api/v1/pnl/equity`
- `GET /api/v1/performance`
- `GET /api/v1/regime`
- `GET /api/v1/latency`
- `GET /stream/v1/dashboard`

The frontend is allowed to do local filtering, sorting, and chart zooming. It
cannot change backend state.

## ReadOnlyGateway

The read-only backend allows only:

- `GET`
- `HEAD`
- SSE stream reads

It rejects write methods:

- `POST`
- `PUT`
- `PATCH`
- `DELETE`

Rejected write methods return `405` with an `Allow: GET, HEAD` header.

The SSE publisher uses a bounded copy-out ring. Slow or disconnected dashboard
clients must not block runtime processing; old frames are dropped when needed.

## Frontend Boundary

The dashboard frontend is an observability plane only.

It must not define or call mutation functions such as:

- `submitOrder()`
- `approveIntent()`
- `cancelOrder()`
- `enableLive()`
- `setRiskConfig()`

It must not use mutation HTTP methods. API access is limited to a GET client and
an SSE client.

Disconnecting or closing the dashboard must not affect backend processing.

## Workflow Commands

Build and verify the paper workflow:

```bash
cmake --build build -j2 --target paper_tools paper_backend paper_backend_tests

build/verify_paper_trading_workflow \
  --execution-reports tests/fixtures/paper/execution_reports_positive.jsonl \
  --reservation-dispositions tests/fixtures/paper/reservation_dispositions.jsonl \
  --snapshots tests/fixtures/paper/market_state_snapshots_mark.json \
  --starting-cash 1000000000 \
  --check-determinism
```

Verify the read-only backend:

```bash
ctest --test-dir build -R "ReadOnlyApi|Sse_" --output-on-failure
```

Build the frontend dashboard:

```bash
cd frontend/dashboard
npm install
npm run build
```

Run the local dashboard skeleton:

```bash
cd frontend/dashboard
npm run dev -- --host 127.0.0.1 --port 5173
```

## Failure Conditions

This layer fails validation if any of the following occur:

- Paper PnL changes from `OpportunityIntent` or `RiskDecision`
- duplicate `ExecutionReport` changes ledger state twice
- unsupported SELL fill mutates portfolio state in v0
- frontend contains mutation API functions
- frontend uses `POST`, `PUT`, `PATCH`, or `DELETE`
- read-only backend accepts write methods
- SSE publishing blocks runtime progress
- dashboard disconnect changes backend state
- determinism check fails

## Current Limitations

- The backend route core is intentionally minimal and read-only.
- The dashboard tables are prepared for future records but currently depend on
  backend read-model availability.
- SELL portfolio accounting is not enabled in v0.
- PnL quality is only as good as the mark snapshots provided to Paper Trading.

## Acceptance

Paper Trading Framework v0 is accepted when:

- `engine_paper` and `paper_tools` build
- read-only backend tests pass
- frontend production build passes
- Paper PnL only comes from execution reports
- frontend cannot mutate backend state
- dashboard remains an observability plane
- workflow determinism passes

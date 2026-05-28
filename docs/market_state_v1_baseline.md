# Market State V1 Baseline

Date: 2026-05-28

This document freezes the Market State v1 correctness baseline before further State Layer changes.

## Test Baseline

```text
ctest: 124 passed
LiveFeedSmokeManual disabled
```

## Fixture Baseline

Fixture:

```text
tests/fixtures/polymarket/market_39.raw
```

Expected replay result:

```text
normalized_events: 39
snapshot_events: 1
delta_events: 35
heartbeat_events: 3
state_errors: 0
entity_count: 1
global_hash: 12959912045291989833
trace_equal: true
determinism passed: true
```

## Module Boundaries

```text
Feed:
  RawPacket / RawLog / Replay source

Decode:
  DecodePipeline -> NormalizedEventBatch

State:
  EntityStateStore -> MarketStateView

ChainConfirm:
  OrderFilled / ClassifiedFill / ConfirmedFillStore
```

## Invariant

```text
same market_39.raw
  -> DecodePipeline
  -> EntityStateStore
  -> MarketStateView
  -> same global_hash
```

Any State Layer change that modifies the replay counts, entity count, trace equality, determinism result, or global hash must be treated as a behavior change and explained explicitly.

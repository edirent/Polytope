# Decode Layer

## Responsibility

The Decode Layer owns this boundary:

```text
RawPacket / RawPacketView -> NormalizedEventBatch
```

It turns raw packet payloads into stable internal normalized events. It is the
only layer that should understand codec selection, JSON/control classification,
venue-specific parsing, and event normalization.

## Non-Responsibility

The Decode Layer does not own:

```text
No WebSocket
No reconnect
No raw log writing
No LOB mutation
No state construction
No signal generation
No order execution
No database writes
```

Decode code must not mutate `EntityStateStore`, write the LOB, call execution,
or reach into signal/risk modules.

## Pipeline

```text
RawPacketView
  -> Codec
  -> JsonDecoder / BinaryDecoder
  -> ExchangeParser
  -> Normalizer
  -> EventPublisher
```

Long-term pipeline shape:

```text
RawPacketView
    -> codecs/
DecodedPayloadView
    -> json/
JsonDecodeResult
    -> parser/
ParsedVenueMessageBatch
    -> normalize/
NormalizedEventBatch
    -> publish/
EventRing / test sink
```

## Current Venue

```text
Polymarket Market WebSocket
```

## Current Required Behaviors

The migrated implementation must preserve the behavior already proven by the
Feed Layer:

```text
PONG/control payload is not malformed JSON.
Array-wrapped payload must be supported.
One packet may produce multiple normalized events.
price_change must use nested price_changes[].asset_id.
Malformed payload must not crash.
Unknown event type must not break pipeline.
```

`price_change` routing is a hard regression target. Real Polymarket payloads
carry top-level `market` and nested `price_changes[].asset_id`. Snapshot and
delta state must target the same asset entity, so the nested asset id is the
correct entity id for deltas.

## Determinism

```text
Same raw log + same decode config = same normalized event sequence
                                   + same decode hash
```

The Decode Layer must preserve packet order unless a future reorder buffer is
explicitly introduced and tested. Step 1 does not add multi-worker decode.

## Boundary

```text
Feed owns transport and raw capture.
Decode owns parsing and normalization.
State owns LOB/entity mutation.
```

Allowed dependency direction:

```text
engine/feed  -> engine/decode/public
engine/state -> engine/decode/public
engine/decode/core -> codecs/json/parser/normalize/publish/metrics
```

Forbidden dependencies:

```text
engine/decode -> engine/state
engine/decode -> engine/signal
engine/decode -> engine/risk
engine/decode -> engine/execution
engine/decode/public -> Boost.Beast
engine/decode/public -> WebSocketClient
engine/decode/public -> EntityStateStore
```

## Directory Responsibilities

### public/

Stable external contract. External modules should depend only on this directory.

Contains decode input/output types, normalized event types, error types, and
abstract interfaces for codec, parser, normalizer, pipeline, and publisher.

### core/

Pipeline orchestration and worker shell.

Step 1 only defines the location. Later steps will wire:

```text
codec -> json/binary decode -> venue parser -> normalizer -> publisher
```

### codecs/

Codec selection and decompression.

`NONE` is the default path. `GZIP`, `LZ4`, and `ZSTD` may remain stubs until
the feed captures compressed payloads. Codec failures should return decode
errors, not throw through the pipeline.

### json/

Payload classification and JSON/control decoding.

This layer does not interpret venue event types. It only classifies payloads as
JSON object, JSON array, non-JSON control, unsupported JSON, or malformed JSON.

### parser/

Venue-specific parsing into parsed venue messages.

Polymarket parser responsibilities include `book`, `price_change`,
`best_bid_ask`, `tick_size_change`, `last_trade_price`, `new_market`,
`market_resolved`, and `PONG`.

### normalize/

Conversion from parsed venue messages into `NormalizedEventBatch`.

This owns price scale, size scale, side mapping, timestamp normalization,
warnings, and event-level validation.

### publish/

Downstream event delivery boundary.

The pipeline should produce a batch and hand it to an event publisher. It should
not know about LOB writers or `EntityStateStore`.

### metrics/

Decode counters and latency buckets.

Metrics should track p50, p95, p99, and max rather than only averages.

### replay/

Decode-only replay:

```text
RawLogReader -> DecodePipeline -> normalized event trace/hash
```

This replay path must not enter state, LOB, signal, risk, or execution.

### internal/

Private decode constants, assertions, limits, and scratch buffers.

External modules must not include `internal/`.

## Golden Fixture

The current Decode golden fixture remains:

```text
tests/fixtures/polymarket/market_39.raw
tests/fixtures/polymarket/market_39.jsonl
```

Expected fixture behavior:

```text
39 packets
36 json_ok
3 non_json
1 book
35 price_change
3 heartbeat
39 normalized events
1 snapshot
35 delta
0 normalization errors
```

## Migration Map

```text
Current location                         Target location

feed/JsonDecoder.*                       decode/json/JsonDecoder.*
feed/EventNormalizer.*                   decode/normalize/Normalizer.*
feed/polymarket normalize logic          decode/parser/polymarket/*
feed/NormalizedEvent.*                   decode/public/NormalizedEvent.*
feed/decode tests                        tests/unit/decode/*
feed/normalize tests                     tests/unit/decode/*
inspect_normalized_events                tools/decode/inspect_normalized_events
run_feed_e2e decode-only subset          tests/replay/decode/*
```

`EntityStateStore` does not move into Decode.

## Step 1 Acceptance

Step 1 is complete when:

```text
engine/decode/ exists.
Decode README defines responsibility and non-responsibility.
public/ contract filenames are fixed.
core/codecs/json/parser/normalize/publish/metrics/replay/internal exist.
engine_decode target exists.
decode test entry directories exist.
market_39.raw is listed as the Decode golden fixture.
price_change nested asset_id is listed as a regression requirement.
PONG/control payload is documented as non-malformed.
Decode explicitly forbids LOB, Signal, Risk, and Execution access.
```

Step 1 does not require full parser implementation, ring integration, metrics
output, or decode-only replay correctness tests.

# Feed Layer Technical Report

## 1. Executive Summary

The Feed Layer implements the market-data ingestion and replay foundation for the trading engine. Its responsibility is not to generate trading signals or execute orders. Its responsibility is to transform an unreliable external WebSocket stream into a reliable, auditable, replayable, and deterministic internal data stream.

The current implementation has completed the first production-like MVP for Polymarket Market WebSocket ingestion. It supports live capture, heartbeat management, reconnect control, binary raw logging, byte-level raw-log validation, JSON decoding, event normalization, entity state construction, deterministic replay, and latency measurement.

The core invariant is:

```text
Live path and replay path must pass through the same decode,
normalize, and state construction logic.
```

This allows the system to treat the raw log as the source of truth. If live behavior is ever questioned, the exact raw packets can be replayed offline to reproduce the state transition trace.

## 2. Scope and Non-Scope

### 2.1 In Scope

The Feed Layer is responsible for:

1. Connecting to Polymarket Market WebSocket.
2. Sending market subscription messages.
3. Maintaining heartbeat through PING/PONG.
4. Detecting source close, source error, and stale feed behavior.
5. Managing reconnect decisions and connection identifiers.
6. Capturing every inbound raw payload.
7. Writing binary append-only raw logs.
8. Writing auxiliary JSONL logs for schema inspection.
9. Validating raw logs byte-for-byte against captured payloads.
10. Decoding raw payloads into JSON/control messages.
11. Normalizing venue-specific payloads into internal events.
12. Building entity-level market state from normalized events.
13. Hashing state deterministically.
14. Replaying raw logs through the same pipeline.
15. Producing E2E correctness and latency reports.

### 2.2 Out of Scope

The Feed Layer does not:

1. Generate alpha or trading signals.
2. Execute orders.
3. Manage positions.
4. Price risk.
5. Perform portfolio accounting.
6. Make strategy decisions.
7. Decide trade sizing.
8. Connect to private authenticated user order streams in the current MVP.
9. Provide HFT-grade kernel-bypass or colocated market-data performance.

Those belong to later layers: MarketStateView, Signal Engine, Risk Engine, and Execution Engine.

## 3. System Architecture

The Feed Layer is organized into six major subsystems:

```text
Feed Layer
|-- Source Runtime
|   |-- WebSocketClient
|   |-- HeartbeatController
|   `-- ReconnectController
|
|-- Raw Ingest
|   |-- RawPacket
|   |-- RawLogWriter
|   `-- RawLogReader
|
|-- Decode
|   |-- JsonDecoder
|   `-- EventNormalizer
|
|-- State
|   |-- EntityStateStore
|   `-- StateHasher
|
|-- Integrity / Recovery / Output
|   |-- ConsistencyChecker
|   |-- StaleDetector
|   |-- RecoveryController
|   |-- EventBus
|   |-- HealthPublisher
|   `-- ReplayRunner
|
`-- Tools
    |-- validate_raw_log
    |-- inspect_polymarket_payloads
    |-- inspect_normalized_events
    |-- run_feed_e2e
    `-- run_live_feed_smoke
```

The design intentionally separates transport, raw capture, decoding, state, and replay. This keeps the system testable and prevents transport-specific logic from leaking into market-state logic.

## 4. Live Data Flow

The live path is:

```text
Polymarket WebSocket
    -> WebSocketClient
    -> on_message(raw_payload)
    -> make_raw_packet(...)
    -> RawLogWriter.write_packet(...)
    -> logs/live_market.raw
    -> optional logs/live_market.jsonl
```

For the full live-plus-state path:

```text
RawPacket
    -> JsonDecoder
    -> EventNormalizer
    -> NormalizedEvent[]
    -> EntityStateStore.apply(...)
    -> StateHasher
```

The live capture path writes raw data first. Decode and state construction are downstream and can be replayed from disk if needed.

## 5. Replay Data Flow

The replay path is:

```text
market.raw
    -> RawLogReader
    -> JsonDecoder
    -> EventNormalizer
    -> EntityStateStore
    -> StateHasher
    -> Replay trace / summary / latency report
```

The replay path deliberately uses the same decoder, normalizer, and state store as the live path. This is the primary correctness mechanism of the Feed Layer.

A valid replay must satisfy:

```text
same raw log + same code = same normalized event sequence
                         + same state trace
                         + same global hash
```

## 6. Source Runtime

### 6.1 WebSocketClient

WebSocketClient is a thin transport wrapper. It provides:

1. `connect()`
2. `run()`
3. `send(message)`
4. `disconnect()`
5. `on_open` callback
6. `on_message` callback
7. `on_close` callback
8. `on_error` callback

It does not parse JSON and does not understand Polymarket-specific event types.

The implementation uses Boost.Beast with TLS for `wss://` endpoints. The public header uses PIMPL to avoid leaking Boost/OpenSSL types into the rest of the project.

### 6.2 HeartbeatController

HeartbeatController tracks source liveness. It maintains:

```text
last_ping_sent_ns
last_pong_received_ns
last_message_received_ns
waiting_for_pong
```

It answers:

1. Should we send PING now?
2. Are we waiting for PONG?
3. Has PONG timed out?
4. Has the source gone stale?

It does not call `WebSocketClient::send()` directly. The runtime owns the action; the controller owns the state decision.

### 6.3 ReconnectController

ReconnectController tracks reconnect state and backoff. It manages:

```text
reconnect_requested
reconnecting
connection_id
reconnect_count
attempt_count
current_delay_ns
reconnect_reason
```

It decides when a reconnect should be attempted but does not perform the actual network operation. The runtime calls `WebSocketClient::connect()` and then reports success/failure back to the controller.

## 7. Raw Ingest

### 7.1 RawPacket

Each inbound payload is wrapped in a RawPacket:

```text
RawPacket
|-- RawPacketHeader
`-- payload bytes
```

The header includes:

```text
magic
version
header_size
recv_wall_ns
recv_monotonic_ns
connection_id
packet_id
source_id
codec
flags
payload_len
payload_crc32
```

The purpose of this structure is to make raw market data:

1. identifiable
2. versioned
3. timestamped
4. source-routed
5. connection-aware
6. length-delimited
7. checksum-validated
8. replayable

### 7.2 RawLogWriter

RawLogWriter writes append-only binary records:

```text
[RawPacketHeader][payload bytes]
[RawPacketHeader][payload bytes]
[RawPacketHeader][payload bytes]
```

It validates packet metadata before writing:

```text
magic
version
header_size
payload_len
payload_crc32
```

### 7.3 RawLogReader

RawLogReader reverses the process:

```text
read RawPacketHeader
validate magic/version/header_size
read payload_len bytes
recompute CRC32
return RawPacket
```

It explicitly reports:

```text
EndOfFile
TruncatedHeader
BadMagic
UnsupportedVersion
BadHeaderSize
TruncatedPayload
CrcMismatch
IoError
```

## 8. Decode Layer

### 8.1 JsonDecoder

JsonDecoder classifies payloads into:

```text
JsonObject
JsonArray
NonJsonControl
UnsupportedJson
MalformedJson
```

Important behavior:

```text
PONG / ping-like control messages are not treated as malformed JSON.
```

This matters because real Polymarket traffic contains non-JSON heartbeat/control messages.

### 8.2 EventNormalizer

EventNormalizer maps Polymarket-specific messages into stable internal event categories:

```text
book              -> Snapshot
price_change      -> Delta
best_bid_ask      -> StatusChange
tick_size_change  -> StatusChange
last_trade_price  -> TradeEvent
new_market        -> LifecycleEvent
market_resolved   -> LifecycleEvent
PONG              -> Heartbeat
unknown           -> Unknown
```

The normalizer supports:

1. object payloads
2. array-wrapped payloads
3. one raw packet -> multiple normalized events
4. non-JSON control payloads

A real issue was found and fixed during E2E testing: `price_change` originally used the top-level `market` field as `entity_id`. Real Polymarket `price_change` payloads contain nested `price_changes[].asset_id`, and the delta must be applied to the asset entity, not the market entity. The fix now prioritizes nested `price_changes[].asset_id` and only normalizes changes belonging to the same asset.

Without this fix, snapshot and delta events would be routed into different entities, corrupting state construction.

## 9. State Layer

### 9.1 EntityStateStore

EntityStateStore consumes only `NormalizedEvent`. It does not read raw payloads or JSON.

It supports:

```text
Snapshot        -> initialize/replace order book
Delta           -> update price levels
StatusChange    -> update metadata/reference fields
LifecycleEvent  -> close/resolve entity
Heartbeat       -> ignored by state
Unknown         -> ignored or counted
```

Core rule:

```text
Delta cannot be applied before Snapshot.
```

If a delta arrives before a snapshot:

```text
entity.status = Recovering
error_count++
book is not mutated
```

### 9.2 Order Book State

The order book state stores:

```text
bids: descending price map
asks: ascending price map
best_bid
best_ask
external_best_bid
external_best_ask
tick_size
crossed flag
resolved flag
winning_asset_id
```

Deterministic maps are used instead of unordered maps to ensure stable replay hashes.

### 9.3 StateHasher

StateHasher computes deterministic FNV-1a based hashes for:

1. OrderBookState
2. EntityState
3. global entity map

It intentionally excludes:

```text
wall-clock time
monotonic time
pointer addresses
unordered iteration order
```

The purpose is replay determinism:

```text
same normalized event sequence -> same final hash
```

## 10. E2E Validation

### 10.1 Raw Fixture

A validated Polymarket fixture is stored as:

```text
tests/fixtures/polymarket/market_39.raw
tests/fixtures/polymarket/market_39.jsonl
```

Validation result:

```text
validated 39 packets
packets_read=39
bytes_read=25988
first_packet_id=1
last_packet_id=39
```

A key detail is that `market.jsonl` physically has 40 lines because one real Polymarket payload contains an internal newline. The validator does not use line boundaries. It uses `RawPacketHeader.payload_len` to perform byte-level alignment. That is the correct validation method.

### 10.2 Payload Inspection

Inspection result:

```text
total_packets: 39
json_ok: 36
non_json: 3
decode_errors: 0
heartbeat_packets: 3
json_events: 36
array_wrapped_packets: 1
max_events_per_packet: 1

type_counts:
  book: 1
  price_change: 35
  best_bid_ask: 0
  last_trade_price: 0
  market_resolved: 0
  unknown: 0
```

This confirmed that array-wrapped payloads exist in real data, even though the current fixture only has one event per raw packet.

### 10.3 Normalized Event Inspection

Inspection result:

```text
total_packets: 39
decode_json_object: 35
decode_json_array: 1
decode_control: 3
decode_errors: 0

normalized_events: 39
snapshot_events: 1
delta_events: 35
status_change_events: 0
lifecycle_events: 0
trade_events: 0
heartbeat_events: 3
unknown_events: 0
normalization_warnings: 0
normalization_errors: 0
```

### 10.4 Offline E2E

Command:

```bash
build/run_feed_e2e tests/fixtures/polymarket/market_39.raw --repeat 10000 --check-determinism
```

Result:

```text
packets_read: 39
decoded_json_object: 35
decoded_json_array: 1
decoded_control: 3
decode_errors: 0

normalized_events: 39
snapshot_events: 1
delta_events: 35
heartbeat_events: 3
unknown_events: 0
normalization_errors: 0

state_events_applied: 36
snapshots_applied: 1
deltas_applied: 35
heartbeats_ignored: 3
state_errors: 0
entity_count: 1
global_hash: 18150466394326955208

latency_samples: 390000
determinism passed: true
trace_equal: true
```

This validates:

1. Raw log can be replayed.
2. Decode succeeds.
3. Normalization succeeds.
4. Snapshot and delta events target the same entity.
5. State construction succeeds.
6. Replay is deterministic.

## 11. Latency Results

### 11.1 Offline Pipeline Latency

Repeated 10,000 times over the 39-packet fixture:

```text
latency_samples: 390000

total_packet_pipeline_ns:
  p50: 87682
  p95: 202009
  p99: 526223
  max: 10084702
```

Interpretation:

```text
p50 ~= 87.7 microseconds
p95 ~= 202 microseconds
p99 ~= 526 microseconds
max ~= 10.08 ms
```

The median and p95 are acceptable for the current public-WebSocket MVP. The p99 and max indicate tail latency from Boost.JSON parsing, dynamic allocations, maps, system scheduling, or measurement noise. This is not yet optimized for low-latency trading, but it is adequate as a correctness-first feed layer.

### 11.2 Live Capture Smoke Test

Command:

```bash
build/run_live_feed_smoke --seconds 300 --asset-id <ASSET_ID>
```

Result:

```text
runtime_seconds: 300
messages_received: 534
bytes_received: 424659
packets_written: 534
ping_sent: 30
pong_received: 30
reconnect_count: 0
raw_write_errors: 0
transport_errors: 0
last_message_age_ms: 799
validate_raw_log_passed: true
```

This confirms:

1. Live connection remained stable for 300 seconds.
2. Heartbeat worked.
3. No reconnect was required.
4. Every received message was written as a packet.
5. Raw log validation passed after live capture.
6. Source was non-stale at exit.

### 11.3 Live Capture Latency

```text
make_raw_packet_ns:
  p50: 39601
  p95: 158171
  p99: 321572
  max: 385991

raw_write_packet_ns:
  p50: 38308
  p95: 247666
  p99: 444886
  max: 1571303

total_on_message_capture_ns:
  p50: 80122
  p95: 409114
  p99: 797580
  max: 1834611
```

Interpretation:

```text
median capture latency ~= 80 microseconds
p99 capture latency ~= 798 microseconds
max capture latency ~= 1.83 ms
```

The main sources of capture cost are:

1. RawPacket construction and CRC32.
2. `std::ofstream` binary write.

This is acceptable for the current MVP. Future lower-latency work should consider:

1. table-based or hardware CRC32
2. batch writes
3. mmap-based raw log
4. async writer thread
5. preallocated buffers
6. avoiding per-message string copies

## 12. Tests and CI Status

The test suite currently includes:

```text
Raw ingest tests
Decode tests
Normalize tests
State tests
Replay tests
Offline E2E fixture test
Live smoke manual test
```

Current result:

```text
100% tests passed
0 tests failed
73 tests total
LiveFeedSmokeManual disabled by default
```

LiveFeedSmokeManual is intentionally excluded from default CI because it depends on external network behavior and Polymarket availability.

## 13. Reliability Fixes

### 13.1 price_change Entity Routing Bug

Problem:

```text
price_change was using top-level market as entity_id.
```

Impact:

```text
Snapshot initialized one asset entity.
Delta updates were routed to a different market entity.
This split state and made the order book invalid.
```

Fix:

```text
price_change now prioritizes nested price_changes[].asset_id.
Only changes belonging to the same asset are normalized into one Delta event.
```

Validation:

```text
entity_count: 1
state_errors: 0
snapshots_applied: 1
deltas_applied: 35
```

### 13.2 Boost.Beast Close During Blocking Read

Problem:

```text
Calling WebSocket close frame while synchronous read() is running can trigger
Boost.Beast assertion.
```

Fix:

```text
disconnect() now closes the underlying socket to interrupt blocking read
safely during shutdown.
```

Validation:

```text
Full test suite passed after fix.
Live smoke completed cleanly.
```

## 14. Current Limitations

The Feed Layer MVP is complete, but it is not final production infrastructure.

Known limitations:

1. Fixture currently covers book, price_change, and heartbeat only.
2. best_bid_ask, tick_size_change, market_resolved, and last_trade_price are implemented but not covered by current live fixture.
3. Public WebSocket latency is not exchange-grade latency.
4. Raw capture still uses `std::ofstream`.
5. CRC32 implementation is correctness-first, not performance-optimized.
6. Reconnect path exists, but the 300s live smoke did not trigger a real reconnect.
7. No authenticated User channel support in this Feed MVP.
8. No multi-source arbitration.
9. No backpressure handling beyond current capture path.
10. No mmap or ring-buffer raw ingest path yet.

These are acceptable for the current stage because the project priority was correctness, replayability, and real-data compatibility.

## 15. Operational Commands

Build:

```bash
cmake --build build -j2
```

Run Full Test Suite:

```bash
ctest --test-dir build --output-on-failure
```

Validate Raw Log:

```bash
build/validate_raw_log \
  tests/fixtures/polymarket/market_39.raw \
  tests/fixtures/polymarket/market_39.jsonl
```

Inspect Raw Payloads:

```bash
build/inspect_polymarket_payloads \
  tests/fixtures/polymarket/market_39.raw
```

Inspect Normalized Events:

```bash
build/inspect_normalized_events \
  tests/fixtures/polymarket/market_39.raw
```

Offline E2E:

```bash
build/run_feed_e2e \
  tests/fixtures/polymarket/market_39.raw \
  --repeat 10000 \
  --check-determinism
```

Live Smoke Test:

```bash
build/run_live_feed_smoke \
  --seconds 300 \
  --asset-id <ASSET_ID>
```

Live smoke writes:

```text
logs/live_market.raw
logs/live_market.jsonl
```

## 16. Engineering Assessment

The Feed Handler has reached a credible MVP state.

It proves:

1. Real Polymarket data can be captured.
2. Raw capture is byte-verifiable.
3. Decode handles real payload shapes.
4. Normalization handles object payloads, array wrappers, and control messages.
5. State construction works on real fixture data.
6. Replay is deterministic.
7. Latency is measured rather than guessed.
8. Live capture is stable for at least 300 seconds under tested conditions.

The most important architectural win is not the WebSocket connection. The important win is this invariant:

```text
The same raw log can deterministically reconstruct the same internal state.
```

That is the foundation needed before building signal generation or execution.

## 17. Recommended Next Step

Do not keep expanding Feed Handler unless a concrete source-level bug appears.

The next layer should be:

```text
MarketStateView
```

It should expose read-only query APIs over EntityStateStore, such as:

```text
get_best_bid(asset_id)
get_best_ask(asset_id)
get_mid(asset_id)
get_spread(asset_id)
get_depth(asset_id)
is_live(asset_id)
is_recovering(asset_id)
is_closed(asset_id)
state_hash(asset_id)
```

This creates a clean boundary between Feed and Strategy.

After that, build:

```text
Signal Engine
Risk Gate
Execution Adapter
```

Do not let strategy code read raw JSON or raw packets directly. Strategy should consume only stable market-state views.

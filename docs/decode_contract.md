# Decode Contract

Decode owns:

```text
RawPacket / RawPacketView -> NormalizedEventBatch
```

Decode must not own:

```text
WebSocket
Reconnect
Raw log writing
LOB mutation
State construction
Signal generation
Risk decisions
Execution
```

Golden behavior to preserve:

```text
PONG/control payload is not malformed JSON.
Array-wrapped payload is supported.
One packet may produce multiple normalized events.
price_change uses nested price_changes[].asset_id.
Unknown event type does not break the pipeline.
Malformed payload does not crash.
```


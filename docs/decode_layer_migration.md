# Decode Layer Migration Map

Step 1 creates the module boundary only. It does not migrate behavior.

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

`EntityStateStore` stays outside Decode. State consumes normalized events and
must not read raw payloads or JSON.


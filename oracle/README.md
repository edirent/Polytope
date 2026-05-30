# Oracle Module

The oracle module will own market rule ingestion, rule approval, candidate
enumeration, payoff compilation, bundle generation, and artifact manifests.

Current scope:

```text
Oracle v0
  fixture market ingestion
  Polymarket Gamma market metadata ingestion
  approved rulebooks
  deterministic artifacts
  optional OpenRouter RuleDraft extraction
  explicit manual approval before compiler use
```

Non-responsibilities:

```text
No automatic rule approval from LLM output.
No runtime LLM calls in the trading hot path.
No runtime compiler or solver execution.
No trading signal generation.
```

`ORACLE_ENABLE_LLM` defaults to `OFF`. Builds and tests must pass without any
LLM API key.

Fetch a real Polymarket market snapshot and conservative structural bundles:

```text
build/fetch_market_universe \
  --polymarket-live \
  --limit 10 \
  --out runs/oracle_live_polymarket/raw_markets.jsonl \
  --candidate-bundles-out runs/oracle_live_polymarket/candidate_bundles.json
```

Optional LLM extraction uses:

```text
OPENROUTER_API_KEY
OPENROUTER_MODEL optional override
OPENROUTER_MAX_TOKENS optional output cap
meta-llama/llama-3.3-70b-instruct:free
https://openrouter.ai/api/v1/chat/completions
```

LLM output is limited to `RuleDraft`. It must be approved through
`ManualRuleEditor` before it can become a compiler-ready `Rulebook`.

If OpenRouter returns HTTP 429 for the default free model, use
`OPENROUTER_MODEL` or `extract_oracle_rules --model` to try another model, or
wait for the provider limit to reset.

Minimal paid smoke with NVIDIA Nemotron:

```text
build/extract_oracle_rules \
  --use-llm \
  --model nvidia/llama-3.3-nemotron-super-49b-v1.5 \
  --max-tokens 256 \
  --market-snapshot tests/fixtures/oracle/raw_markets_small.jsonl \
  --drafts-out runs/oracle_rule_drafts.json
```

For real combinatorial extraction, prefer one `event_id` at a time. The prompt
groups markets by event and explicitly rejects trivial single-market
`ExactlyOne(Yes, No)` drafts:

```text
build/extract_oracle_rules \
  --use-llm \
  --model nvidia/llama-3.3-nemotron-super-49b-v1.5 \
  --max-tokens 10000 \
  --event-id 24383 \
  --market-snapshot runs/oracle_live_polymarket_2000_paginated/raw_markets.jsonl \
  --drafts-out runs/oracle_live_polymarket_2000_paginated/rule_drafts_24383.json
```

After manual approval, combinatorial bundle generation should run from the
approved Rulebook rather than from the raw market metadata stage:

```text
build/verify_oracle_workflow \
  --market-snapshot runs/oracle_live_polymarket_2000_paginated/raw_markets.jsonl \
  --rulebook runs/oracle_live_polymarket_2000_paginated/rulebook_24383_approved.json \
  --generate-combinatorial-bundles \
  --out runs/oracle_live_polymarket_2000_paginated/artifact_24383 \
  --check-determinism
```

The Rulebook-driven generator intentionally skips trivial single-market rules.
For cross-market rules it generates:

```text
ExactlyOne(YES_1..YES_N) -> buy all YES, guaranteed payout = 1
ExactlyOne(YES_1..YES_N) -> buy all NO, guaranteed payout = N - 1
AtMostOne(YES_1..YES_N)  -> buy all NO only, guaranteed payout = N - 1
```

It does not generate the dangerous `AtMostOne` YES basket, because an outside
winner can make every listed YES resolve to zero.

Large rulebooks no longer have to pass through one global brute-force state
enumeration. `verify_oracle_workflow` now builds a constraint graph, partitions
it into connected components, and uses semantic component oracles for supported
large constraints:

```text
ConstraintCompiler
  -> ConstraintGraphBuilder
  -> ComponentPartitioner
  -> SmallEnumOracle / ExactlyOneOracle / AtMostOneOracle
```

For a large component such as the World Cup winner `AtMostOne` rule, the
workflow reports `enumeration_mode: component_oracle` and writes component /
oracle descriptor payloads into the artifact. Full payoff matrices remain only
for small enumerable components.

# Oracle Module

The oracle module will own market rule ingestion, rule approval, candidate
enumeration, payoff compilation, bundle generation, and artifact manifests.

Current scope:

```text
Step 12.1
  skeleton only
  public manifest/error types
  CMake targets
  no live LLM calls
  no network access
```

Non-responsibilities in this step:

```text
No rule extraction implementation.
No live LLM provider integration.
No candidate enumeration.
No payoff math.
No artifact IO.
No trading signal generation.
```

`ORACLE_ENABLE_LLM` defaults to `OFF`. Builds and tests must pass without any
LLM API key.

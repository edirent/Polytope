# Trading Engine

Low-latency trading engine skeleton with separate runtime, oracle, replay, research, testing, and tooling surfaces.

## Layout

```text
config/      Market, risk, runtime, and feed configuration.
engine/      C++ runtime engine implementation.
oracle/      Rule extraction, constraint compilation, enumeration, and artifacts.
replay/      Recorder, player, deterministic runner, and reports.
research/    Notebooks, optimization, experiments, and validation.
tests/       Unit, integration, replay, fuzz, and performance tests.
tools/       Operator and developer utilities.
```

## Runtime Pipeline

```text
feed -> decode -> state -> signal -> risk -> execution
```

The runtime path should stay deterministic, observable, and isolated from research-time mutation. Replay and oracle outputs should enter the engine through explicit configuration or artifacts.

## Build

```bash
cmake -S . -B build
cmake --build build
```

BTC 15-minute lead-lag maker strategy and deterministic delayed replay:
[configuration, frozen model contract, and usage](docs/btc15m_lead_lag.md).
Public PMXT v2 historical-data backtest: [coverage audit and reproduction](docs/pmxt_btc15m_backtest.md).
Frozen shock-entry validation: [assumptions and confirmation window](docs/pmxt_shock_fixed_tests.md).
Cross-venue V0 (Kalshi + international Polymarket CLOB): [offline implementation and audit boundaries](docs/cross_venue_v0.md), [synthetic demo](research/cross_venue/README.md).
Cross-venue V0.1: [native real-data capture, latency replay, and audit](docs/cross_venue_v0_1.md).
Cross-venue V0.1.1: [ranked human-review dossiers, feed diagnostics and real-data evidence](docs/cross_venue_v0_1_1.md). Status: `PARTIALLY_VERIFIED`; no human-approved pairs or live orders.
Authenticated Kalshi WS: [completed 10-minute dual-venue L2 capture, recovery and audit](docs/cross_venue_authenticated_l2.md). Economic approval gates remain active.

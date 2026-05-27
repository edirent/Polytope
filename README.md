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

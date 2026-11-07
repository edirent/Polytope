# Production migration plan and runbook

The first vertical slice is delivered. Remaining stages are recommendations, with explicit acceptance gates. No stage authorizes real-money execution.

## Module disposition

| Current module | Disposition | Concrete work / acceptance gate |
|---|---|---|
| `engine/main.cpp`, `engine/runtime/Runtime.cpp` | EXTEND | Now the accountable entry and supervisor. Keep bounded config-driven modes; add network feed source behind `RawPacket`, measured callback budgets and bounded shutdown. |
| `engine/runtime/DurableJournal.*` | RETAIN / EXTEND | Keep durable pre-submit/report ordering and verified recovery. Add retention/compaction, corruption repair tool, external head anchoring and validated software upgrade snapshots. No silent repair. |
| `engine/runtime/{ThreadManager,CpuAffinity,Shutdown}.cpp` | RETIRE unused placeholders after deployment evidence | Do not activate threads by filename. Keep single writer until measured throughput requires them. |
| `feed/RealtimeFeedEngine`, `source_runtime`, raw reader/writer | RETAIN / EXTEND | Reuse authentication, reconnect and raw provenance. Route actual connection epochs and failures into supervisor. No trading until complete fresh snapshot recovery. |
| `feed/decode/*`, `feed/state/*`, legacy top-level state paths | CONSOLIDATE | Keep capture compatibility; make core `DecodePipeline` + `MarketStateStore` the only mutation path. Confirm every old runner before removing legacy APIs. |
| `decode/core`, venue normalizers | RETAIN | Keep fixed-point/sequence semantics. Reject unsupported compressed protocols until implemented and tested; never mask failures as liquidity absence. |
| `state/core`, `state/book`, `state/view`, `state/cross_venue` | RETAIN / CONSOLIDATE interfaces | Preserve synchronized/as-of guarantees behind a common venue-qualified read facade; one authoritative book per key. Differential replay gate before adoption. |
| `signal/SignalEngine`, `order_decision/OrderDecisionEngine` | RETAIN | Active combinatorial plugin now uses them unchanged. Preserve proof, decision, snapshot and policy hashes. |
| `strategy/market_making`, product engines, fair and rewards | RETAIN / ADAPT | Implement a maker plugin emitting existing quote/cancel types. Do not translate passive quotes to taker plans. Require unchanged quote outputs on frozen inputs. |
| `strategy/cross_venue`, approved pair/payoff artifacts | RETAIN / ADAPT | Implement proof-bearing pair plugin after wallet authority and OMS pair children exist. Explicit human approvals remain required. Historical research intent is not authorization. |
| `risk/core`, guards, quote evaluator, `ReservationBook`, multi-wallet extension | CONSOLIDATE ownership / EXTEND | One portfolio admission facade over existing policies. Reserve actual sized lots, account for filled inventory/cash, replace/cancel-pending/unknown orders and wallet/asset haircuts. Gate: concurrent strategies cannot reuse cash or inventory. |
| `execution/core/ExecutionGateway`, plan/state/cancel/fill modules | RETAIN / EXTEND | Consolidate direct unmanaged submit and maker lifecycle into gateway, remove inferred ACK, add normalized private events, authoritative fill IDs and query-before-retry. Existing adapter functionality stays. |
| Paper, maker and future venue adapters | RETAIN behind central OMS | New executable only constructs PaperAtomic. Add async/maker tests before enabling them here. Live configuration remains refused. |
| `PairExecutionStateMachine` | RETAIN research; ADAPT coordinator | Reuse residual-exposure semantics; orders belong to OMS. Pair interpreter cannot own live order truth. |
| `paper/PaperLedger`, `PositionLedger`, PnL engines | RETAIN | Sole accounting postings; retain PnL attribution with explicit valuation/terminal/receipt labels. Add canonical multi-wallet access and reconciliation. |
| `PaperPortfolio`, runner-local ledgers, `LeadLagReplay::Ledger` | CONSOLIDATE projections; retain research simulators | PaperPortfolio becomes a derived mark projection, not another position authority. Research ledgers remain isolated counterfactual models, not parallel live accounts. |
| `chain_confirm/FillReconciler` | RETAIN / EXTEND service boundary | Keep chain/public-hint reconciliation. Add private exchange account reconciler separately; require receipt-backed order/cash consistency and source timestamps. |
| `engine/metrics` placeholders | CONSOLIDATE into existing snapshots | Add measured histograms and bounded nonblocking publication only after baseline latency is recorded. Do not create another metrics ownership plane. |
| `apps/paper_backend`, dashboard client and views | RETAIN / EXTEND | Current read-only snapshot integration works. Add stale publisher alarms, account mismatch panels and operational tests. Preserve default BTC research UI. |
| World Cup/maker/shadow research runners | CONSOLIDATE orchestration | Convert to config profiles/plugin producers after parity; keep verifier/capture/research executables. Do not delete working research tools before replacements prove equivalent. |
| `config/runtime.yaml` | RETIRE as assumed runtime contract | It is not loaded by the new executable. Use explicit versioned `config/trading_runtime.replay.json`; add migrations instead of silent defaults. |
| Oracle artifacts and research source data | RETAIN / FREEZE | Runtime consumes approved immutable artifacts and checksums. No LLM, discovery approval or payoff optimization inside trading callbacks. |
| Boost.JSON implementation dependency | CONSOLIDATED now | `polytope_json` is shared by Decode and Oracle; unified linking no longer needs duplicate symbol workarounds. |

## Sequence and release gates

1. **Delivered:** one recorded-feed, single-account combinatorial paper/shadow runtime; durable journals; conservative recovery; shared existing strategy/risk/OMS/ledger; operations snapshot; fault tests. Baseline remains synthetic and bounded.
2. **Next:** central OMS typed maker intents, actual-sized reservations and one portfolio facade shared with combinatorial strategy. Require maker parity tests, partial fills, replacement races, cancel-pending collateral and kill coverage before enabling that plugin.
3. **Then:** venue-qualified state facade and cross-venue wallet/pair integration. Preserve pair approvals, per-market fees, fractional quantity precision, settlement-asset budgets and as-of freshness. Require residual-hedge and reconciliation faults; do not create another EMS.
4. **Then:** bounded network PAPER and SHADOW with reconnect/state barriers, private read-only account reconciliation and explicit incident escalation. Public market capture alone cannot pass the account gate.
5. **Operational hardening:** durable compaction/checkpoints, software-version migrations, receipt IDs, watchdog/latency budgets, controlled restart and storage-failure drills. Measure before moving publication off the critical path or adding processes.
6. **Live readiness is outside this task:** no configuration enables it. Any future authorization requires a separate review of exchange receipts, cancellation, reconciliation, funds controls and recovery. Existing adapters being present is not permission.

## Reproducible local startup

Run from repository root. Dependencies are the existing C++20/CMake/Boost/OpenSSL/Python environment; frontend dependencies are in its existing package.

```bash
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure -j 4
build/trading_runtime --config config/trading_runtime.replay.json
```

The default profile uses only two synthetic book records and the existing Oracle exporter-generated YES/NO complete-set fixture in `tests/fixtures/runtime/oracle_artifact`. It writes `runs/runtime_foundation/demo_replay/`. A used output directory is never silently overwritten: rerun with `--recover` or choose a new directory in a new profile. `build/trading_engine` is the same entry implementation and requires the same `--config` argument; the previous no-argument feed-only behavior is intentionally retired.

Generate PAPER and SHADOW profiles without changing strategy or risk interfaces:

```bash
python3 - <<'PY'
import json
from pathlib import Path
base = json.loads(Path('config/trading_runtime.replay.json').read_text())
for mode in ('PAPER', 'SHADOW'):
    cfg = dict(base, mode=mode, output_dir=f'runs/runtime_foundation/demo_{mode.lower()}')
    Path(f'/tmp/polytope-runtime-{mode.lower()}.json').write_text(json.dumps(cfg, indent=2)+'\n')
PY
build/trading_runtime --config /tmp/polytope-runtime-paper.json
build/trading_runtime --config /tmp/polytope-runtime-shadow.json
```

All profiles require schema/version, explicit `live_enabled=false`, finite cash/exposure/inventory/record limits and frozen input/artifact. Unsupported modes/plugins are errors. Supported feeds: JSONL envelopes with positive nondecreasing `recv_ns` and venue-native `payload`; or existing binary raw logs via `input_format=raw`, with CRC validation, uncompressed Polymarket source only. JSONL controls `tick`, disconnect/reconnect, stop/kill and paper account snapshots are explicit recorded events. Test crash hooks require `allow_test_faults=true` and are off in the normal profile.

Regenerate the fixture using existing Oracle functionality:

```bash
build/verify_oracle_workflow --market-snapshot tests/fixtures/runtime/markets.jsonl \
  --rulebook tests/fixtures/oracle/artifact_positive/rulebook.json \
  --candidate-bundles tests/fixtures/runtime/candidate_bundles.json \
  --out /tmp/polytope-runtime-oracle-check --check-determinism
```

## Shutdown and recovery

The runtime exits on bounded EOF. For a longer configured input, SIGINT/SIGTERM stops at the current transaction boundary; it persists the commit and STOP record. `kill -TERM PID` is a drain request, not external order cancellation. A configured kill-switch file is checked between transactions. A recorded stop/kill or mismatch latches the supervisor across restart; merely deleting the file does not resume that journal.

```bash
build/trading_runtime --config config/trading_runtime.replay.json --recover
```

Recovery validates the full chain and exact configuration/input/artifact/executable hashes, reconstructs committed inputs without appending duplicate fills, and continues only if there is no unresolved transaction or stop latch. Exit 0 means bounded execution/drain complete; 1 means startup/config/corruption/runtime failure; 2 means a deliberate supervisor/recovery block. Crash tests use 86/87 only when explicitly enabled.

For exit 2, preserve `events.jsonl`, `frozen_config.json`, the original feed, rule artifacts and executable. Inspect the last INPUT_BEGIN/COMMIT, RISK_RESERVED, SUBMIT_STARTED, PLAN_PREPARED and EXECUTION_REPORT entries. The dashboard exposes known restored receipts and pending recovery encumbrance separately. Do not delete the tail, infer cancellation from elapsed time, or restart in a new directory to pretend an unknown external order disappeared. Automatic resolution of a partial transaction is deliberately not implemented; production needs authoritative reconciliation and a reviewed repair tool first.

## Operations view

In separate terminals after generating evidence:

```bash
build/paper_backend --dashboard-file runs/runtime_foundation/demo_replay/dashboard.json --host 127.0.0.1 --port 8080
cd frontend/dashboard
npm run dev
```

Open `http://127.0.0.1:5173/?view=runtime`. The Vite proxy routes the existing read-only API to localhost:8080. The normal dashboard entry retains the BTC research monitor. The operations page shows recorded feed source, stopped/running lifecycle, integrity/freshness, intents, risk, order states, ledger balances/positions, existing paper PnL, local processing latency and incidents. Stopped recorded snapshots are not a live healthy-feed claim. Backend writes remain rejected. No execution control is exposed.

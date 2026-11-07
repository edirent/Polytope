# Operational trading runtime V0.2.0

Date: 2026-11-06 (America/Chicago). Current code and executable evidence take precedence over older architecture reports. This is an operational paper/shadow foundation, not a claim of production readiness.

## Delivered implementation

The existing `runtime::run` and Session now support live sources. Both `trading_engine` and `trading_runtime` use that implementation; there is no new strategy engine, risk engine, OMS or balance authority.

```text
existing WebSocketClient + existing Kalshi signer
  → RawPacket provenance + bounded venue callback queue
  → single local availability sequencer
  → existing Kalshi/CLOB normalizers and ResearchState/SynchronizedBookStore
  → read-only venue-qualified facade and existing As-Of checks
  → existing SignalEngine combinatorial plugin
  → existing OrderDecisionEngine + RiskEngine/portfolio guards
  → existing ExecutionGateway prepare/persist/submit
  → existing PaperExecutionAdapter
  → durable simulated receipt → existing PaperLedger/PnL
  → existing paper_backend and runtime dashboard
```

ResearchState is reused only as the existing decoder/sequence-aware book owner; its research opportunity scanner and risk probes are not run. The live path does not also mutate MarketStateStore. Recorded schema-v1 regression profiles retain their original MarketStateStore path. The facade returns value projections for the native strategy interfaces and a qualified `read(VenueInstrumentKey)`; it cannot expose a second mutable book authority. Kalshi aliases use the full instrument identity; Polymarket token aliases remain unambiguous.

The runtime currently activates only the existing combinatorial plugin. Real validation uses a same-condition Polymarket YES/NO full-set paper artifact; Kalshi is independently reconstructed/monitored. This does not approve a Kalshi–Polymarket settlement pair or enable cross-venue trades. Market making remains the next plugin, not a falsely enabled current feature.

## Versioned modes

| Profile | Source and behavior |
|---|---|
| Schema 2 PAPER | Live Kalshi/Polymarket feeds; existing strategy/decision/risk path; simulated execution and ledger only. |
| Schema 2 SHADOW | Same live feed and decision/risk auditing; durable prepared/would-submit plan; no adapter invocation or fill. |
| Schema 2 REPLAY | Immutable `venue_journal` input; identical logical clock, strategy/risk and chosen `replay_execution_policy`. |
| Schema 1 recorded profiles | Existing experiments/regressions retain recorded semantics, including `config/trading_runtime.recorded_paper.json`. |
| LIVE | Rejected before activation. No live order adapter is instantiated or linked into the runtime's reachable order path. |

Schema 2 real profiles require finite duration, record budget, finite funded cash/exposure/inventory limits, supported market metadata, frozen Oracle artifacts and explicit `live_enabled=false`. `recorded_fault_fixture` is an explicit test-only source exception. Unsupported protocols and malformed market/asset identities are rejected; no URL/order endpoint can be injected through selected market identifiers.

## Feed and state lifecycle

Per venue, socket connection and subscription traffic are diagnostic states. Actual native snapshots and valid updates establish book reconstruction. Kalshi retains SID/sequence/exchange-clock handling; Polymarket retains its snapshot/absolute-delta reconstruction. Exact repeated market frames are suppressed within a bounded recent-frame window. Reconnection uses a new epoch and invalidates old books; snapshots must restore eligibility. Connection errors invalidate immediately, without waiting for a later close callback.

The main consumer assigns monotonically ordered availability/decision time. Original callback receive time is preserved as the freshness timestamp: queue delay cannot make old data appear new. The facade retains original source provenance, content/version hashes and uses existing `joint_book_asof` validation. Native read projections can use the actual latest authoritative validation time; they do not invent a new source update. Independent 250 ms timer inputs detect stale/quiet failures even when no market callback arrives.

Public REST snapshots are diagnostic reconciliation only. They cannot seed a WebSocket delta stream or turn intermittent REST sampling into continuous L2. Existing quiet-request reconciliation distinguishes an in-flight/older snapshot from a proven mismatch. Mismatches invalidate and request snapshot recovery through the existing WebSocket client.

The bounded queue has item and byte limits; overflow fails closed and is not silently treated as a healthy book. This implementation uses one serialized consumer, thread-safe callback ingress, existing transport workers, bounded publisher history and a bounded intent-ID set. Data and order durability are separated as specified in `oms_durability_audit.md`.

## Operations and accounting

The existing backend reads atomic `dashboard.json`; `/api/v1/runtime` and existing market/intent/risk/snapshot endpoints expose actual state. The runtime operations view is `?view=runtime`. It shows release/configuration versions, connection diagnostics, per-book admission and age, strategy callbacks/intents, risk decisions, order lifecycle, paper cash/reservations/positions, marked versus realized PnL, journal/durable heads, reconciliation state, percentile latency and incidents/stop latch. The backend marks a stale RUNNING publisher DEGRADED. All APIs/UI remain read-only.

`health_history.jsonl` records approximately one-second derived health samples for operational coverage analysis. It is not financial authority. A live socket does not imply an admitted book. EOF/controlled shutdown is STOPPED; alive invalid/unknown state is DEGRADED. Unknown receipts, stop latches or reconciliation mismatches do not auto-clear economic exposure.

Existing PaperLedger handles cash/positions and separate fee postings. Simulated own receipts, public market trades and future private exchange fills are distinct. The read-only private-account interface has no authenticated implementation in this release. No simulated or conditional PnL is verified exchange-realized cash.

## Validation and evidence

Use `runs/runtime_v020/` evidence, especially the final release/validation summary and each experiment's `operational_audit.json`, `operational_report.md`, `replay_comparison.json`, `latency.json`, journals and recording manifest. Actual duration, native market identities, message types, reconstruction counters, reconnect epochs, sampled health coverage and simulated accounting are reported from files, not inferred from requested duration.

The initial synchronous diagnostics were stopped early after measured queue accumulation. They remain labeled development diagnostics. A subsequent 60-second public smoke showed an approximately 0.353 ms median processing time with batched non-economic audit persistence and zero queue depth at completion; this was a different workload/window, not a controlled speedup claim. Final 30-minute PAPER and SHADOW metrics supersede smoke measurements.

Authenticated Kalshi success requires actual orderbook_snapshot/orderbook_delta receipts. Missing credentials, a healthy TCP connection or a signed request is insufficient. Final acceptance remains incomplete if authenticated data, reconstruction/recovery, replay parity or the bounded experiments fail. No 72-hour soak is claimed.

### Completed observation, 2026-11-06

The frozen executable `e43d3e3444789c3304009667017851120bdd6c542bc1f0faa26952cfcda477b1` ran independent simultaneous sessions starting at 18:35:06 America/Chicago. PAPER observed 1,800.133807384 seconds and 46,190 inputs; SHADOW observed 1,800.100215902 seconds and 45,943 inputs. Both exited normally and both credential-free replays reproduced every transaction decision trace and accounting digest. See [the evidence report](../runs/runtime_v020/evidence_20261106/report.md) and [machine-readable summary](../runs/runtime_v020/evidence_20261106/summary.json).

| Evidence | PAPER | SHADOW |
|---|---:|---:|
| Kalshi snapshots / deltas | 2 / 11,437 | 3 / 11,436 |
| Polymarket books / price_change objects | 417 / 17,727 | 414 / 17,515 |
| Runtime input callbacks / risk-decision audit records | 46,190 / 42,716 | 45,943 / 42,474 |
| Both selected markets admitted, availability-event reconstruction | 1,363.529803 s (75.7574%) | 1,360.816369 s (75.6085%) |
| Approximately 1 Hz sampled HEALTHY / DEGRADED | 1,357.761 / 442.101 s | 1,359.764 / 440.056 s |
| Approvals / simulated receipts / submissions | 0 / 0 / 0 | 0 / 0 / 0 |
| Maximum sampled queue depth / dropped at shutdown | 11 / 0 | 4 / 0 |

The selected Kalshi market was `KXBTCD-26NOV0717-T76249.99`; Polymarket was Gamma `6667467`, condition `0xc52d0ad0d001293473fa50126015c9d6db3baab756002f7b4ccf9eb179b1c403`, "US x Iran effective ceasefire continues through November 8, 2026?". They are independent feed probes, not an economically equivalent pair. The existing strategy evaluated the Polymarket same-condition full-set artifact and found no profitable opportunity. Callback totals include timer/diagnostic inputs, not only exchange updates.

Kalshi book streams began with snapshots and had zero observed nonconsecutive book sequences or regressing exchange clocks within each epoch/SID. PAPER used 2 epochs, SHADOW 3. Both recovered after the configured reconnect; SHADOW also recovered an early invalid-time diagnostic event. Periodic authority expiry explains most Kalshi admission interruptions: the 15 s authority budget equals the diagnostic polling interval, so response time and inconclusive comparisons leave conservative gaps. The policy was not loosened to improve coverage.

Each Polymarket session used 23 epochs. Each had 20 primary crossed/locked reconstruction frames (40 per-outcome diagnostic records); subsequent rejected deltas were a consequence of invalidation, not proof of packet loss. PAPER had 1 authoritative REST comparison mismatch; SHADOW had 2 source time/hash rejection records per venue and 2 timeout events. Some rejected price_change frames advertised uncrossed wire BBO while the reconstructed levels crossed. Root cause remains unresolved. Invalidated books were denied reads and rebuilt from new snapshots. Complete continuous correct L2 is not certified by receipt/replay success.

Real PAPER consumer processing latency p50/p95/p99/p99.9 was 0.350/13.673/18.429/26.991 ms. The legacy-named `ws_receive_to_decode` series was 0.454/23.166/54.301/84.673 ms, but pools all venue-packet callbacks including REST and control traffic; it is not an isolated WS-only series. `end_to_end` measures consumer transaction processing, excluding ingress/raw persistence and dashboard; `book_update_to_strategy` includes decoder work and overlaps another stage. Do not sum stage percentiles. The report provides WS market-only callback-to-sequencer measurements and callback-to-commit quantile bounds rather than inventing a combined percentile. Server timestamp differences are negative on this unsynchronized host and are not one-way network latency. Order-specific stages had no real samples; the separate synthetic benchmark had 199 approved paper plans and must not be described as real-market trades or profit.

Full CTest: 1,039 registered, 1,034 executed, 1,033 passed, 1 known pre-existing maker failure, 5 disabled. The operational suite passed all 42 scenarios (including inherited regressions), native lifecycle passed 21 assertions, and TypeScript/Vite passed. All runs are stopped; no indefinite capture remains. Realized simulated PnL, fees, positions and reservations are zero; funded cash remains 100,000,000 ticks. Private reconciliation is UNCONFIGURED and `verified_live_realized_pnl=null`.

## Startup and replay

Generate exact public validation inputs using the existing Oracle exporter and explicit market choices:

```bash
python3 tools/prepare_runtime_validation.py --out runs/runtime_v020/my_validation \
  --kalshi YOUR_ACTIVE_BINARY_TICKER --polymarket-id YOUR_ACTIVE_YES_NO_GAMMA_ID
build/trading_runtime --config runs/runtime_v020/my_validation/paper.json
build/trading_runtime --config runs/runtime_v020/my_validation/shadow.json
```

Use the configured local WSL terminal for authenticated reads. If Codex lacks the terminal environment, `python3 tools/launch_runtime_validation.py` stages credentials only in local process memory and waits for the frozen release-ready manifest. It launches two bounded independent validation experiments and then audits/replays them. It never writes credentials to configuration, arguments or log files.

For a completed frozen experiment:

```bash
python3 tools/audit_operational_run.py --run PATH_TO_RUN --binary PATH_TO_FROZEN_RUNTIME --with-replay
build/paper_backend --dashboard-file PATH_TO_RUN/dashboard.json --host 127.0.0.1 --port 8080
cd frontend/dashboard && npm run dev
```

Open `http://localhost:5173/?view=runtime`. To inspect a new run, point the existing backend at that run's snapshot instead of a previous recorded demonstration. A previous backend process is not automatically replaced.

SIGINT/SIGTERM drains the current transaction and records shutdown. A kill file stops admission and latches across restart. `--recover` requires the same executable/config/artifact/input identity, verifies committed event/account digests and freezes unknown tails. A live restart starts a new epoch only within its original bounded wall-clock window; an already completed window does not silently become an indefinite daemon.

## Next plugin: market making

Retain MarketMakingEngine, product fair/quote logic, QuoteIntent/CancelQuoteIntent, QuoteRiskEvaluator and existing maker adapters. Extend the existing gateway with typed maker lifecycle requests and authoritative asynchronous reports; do not convert passive quotes into taker bundle orders. Before activation, make actual-sized quote reservations global across plugins, retain cancel-pending/unknown collateral, reconcile marks to canonical positions, add replacement/partial-fill races and prove output parity on frozen maker inputs. Preserve the independently existing `AvalancheSkewsQuotesAndRiskCutsBuyChannel` failure as a visible blocker rather than adjusting unrelated strategy behavior here.

## Production-unverified limits

Only one combinatorial plugin and one synthetic paper account are active. Maker/pair execution, private balances, live exchange receipt/cancel/settlement confirmation, multi-wallet settlement-asset risk, venue fee changes and software-upgrade/cross-host recovery are not production-validated. Existing legacy OMS paths are not enabled for real money. Journals have bounded-run budgets but no production compaction or multi-day retention implementation. A blocked kernel fsync/network connect can extend drain time; no hard real-time guarantee is claimed. A monotonic clock reset across host reboot needs a reviewed recovery epoch rather than pretending old timestamps are current.

Legacy schema-v1 aggregate fields such as `feed_connected`, `reconnects`, `fresh_snapshot_assets`, `decode_errors` and `state_errors` do not summarize the venue facade and can disagree with live source state. For schema-v2 operations use `runtime.source_health`, `runtime.venue_counters`, `markets[].admitted` and `runtime.health`; final `source_health` correctly shows both venues disconnected and health STOPPED. Consolidating these legacy display fields is outstanding; they must not be used for an operational readiness gate.

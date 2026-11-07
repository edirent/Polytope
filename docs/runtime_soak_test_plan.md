# Bounded runtime soak procedure

This is a reproducible procedure, not evidence that a 24/72-hour soak has run. The implemented validation window is configured and reported separately with actual elapsed time.

## Before a longer run

Choose supported, liquid binary markets whose trading/settlement horizon exceeds the entire intended observation window. Freeze exact venue identifiers, public metadata, same-condition paper Oracle artifacts and fee/quantity assumptions. Do not silently substitute expired markets or approve a cross-venue pair from title similarity.

Use an independently verified release executable. Verify explicit LIVE disablement and mode, finite duration/record budgets, queue capacity, available disk space, journal retention expectations and kill-file path. Authenticate only from the locally configured terminal. A missing Key ID must not turn REST diagnostics into an authenticated WebSocket claim.

## Stages

| Stage | Window | Admission gate |
|---|---|---|
| Smoke | 1–5 minutes | Actual snapshots/deltas on each configured venue; correct identity/sequence/freshness; no real order adapter. |
| Initial validation | 30 minutes PAPER and SHADOW | Controlled reconnect, recovery snapshots, strategy/risk callbacks, paper accounting, recorded replay parity and measured latency. |
| Extended check | Explicitly requested 2–6 hours | Stable queues/storage/latency and no unexplained exposure; inspect all health incidents first. |
| Long soak | Explicitly requested 24 hours, then 72 hours | Prior stages pass, contracts remain active, storage budgets/rotation and operator coverage are reviewed. Current journal compaction and multi-day operational limits remain unvalidated. |

For a manual bounded profile, copy a generated PAPER/SHADOW configuration, set `duration_seconds` explicitly, use a new output directory and appropriately reviewed `max_records`/queue and storage budgets. The source rejects more than 72 hours and never defaults to an indefinite background run. Do not run the later stages merely because this plan exists.

## During collection

Observe the existing dashboard's release/configuration identity, source connection diagnostics, per-book admission/freshness and health. Check queue depth/bytes, overflow/drop counters, raw/financial durable heads, reservations, paper cash, positions and incidents. Alive-but-invalid must be DEGRADED. A healthy socket cannot restore a failed book; new snapshots/validated reconstruction must do so.

Perform one configured controlled reconnect inside the bounded window. Preserve its disconnect/new epoch/snapshot sequence and the interval without admission. Independent timer records must continue when the market is quiet. Do not inject fills from public trades. SHADOW must retain zero adapter submissions/receipts. Unexplained economic state, mismatches, disk errors or backpressure overflow stop admission; they are not reasons to release funds or infer an exchange cancel.

Do not compare intermittent REST responses as if they were a synchronized continuous L2 stream. Record sampling/reconciliation failures and unavailable coverage explicitly. Polymarket public message silence is not a measured count of lost messages without an exchange sequence.

## Stop and recover

Let the finite window end or send SIGTERM for a controlled drain. Preserve all journals, immutable inputs, public metadata/artifacts, exact executable and configuration. Audit both journal chains and every committed transaction. Run offline REPLAY without credentials, comparing full decision and accounting digests. Inspect known receipt recovery and conservative encumbrances for incomplete transactions.

A stop/kill/reconciliation latch remains latched. Unknown ACK requires authoritative reconciliation before retry; this release's private exchange reconciler is unconfigured. Do not bypass the latch by deleting the journal or funding a new paper account to pretend unknown external exposure vanished.

## Required report

Report actual start/end/duration and market identities, authenticated Kalshi snapshots/deltas, Polymarket books/deltas, valid reconstruction counts, epochs and reconnect recovery, approximately 1 Hz sampled healthy/degraded coverage, message silence/gap limitations, strategy/risk decisions, simulated receipts/fees/cash/positions/marked PnL and replay equality. Include p50/p95/p99/p99.9 only with disclosed sample counts; absent order approvals mean order-stage latency is not observed in that real run.

Distinguish tested local failures from untested kernel/device stalls, public-market state from private account truth, and marked/conditional/simulated PnL from exchange-realized PnL. `verified_live_realized_pnl` remains null without real receipts; this task enables no exchange orders. Passing a soak is evidence for a release gate, not permission to trade real money or a production-ready label.

## Current gate after the completed 30-minute runs

PAPER observed 1,800.134 seconds; SHADOW 1,800.100 seconds. Both received authenticated Kalshi snapshots/deltas and actual Polymarket books/updates, recovered controlled reconnections, exited normally and replayed identically. No economic opportunity was approved; all cash remained funded paper cash and no real orders were submitted.

Do not advance automatically to 24/72 hours. Both sessions required repeated Polymarket snapshot rebuilds; reconstructed joint admission was only 75.76%/75.61%. Investigate the saved crossed/locked-depth versus advertised-BBO examples, the PAPER authoritative REST mismatch, source-time startup rejections and SHADOW timeouts. Review the 15 s authority budget versus 15 s REST polling/response timing, without relabeling diagnostic snapshots as continuous L2. Synchronize/measure host clock offset before interpreting server timestamps. Consolidate legacy telemetry summary fields, inspect the known maker regression, and retain frozen recordings for each proposed fix. See `runs/runtime_v020/evidence_20261106/report.md` for counts and evidence links.

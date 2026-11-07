# Runtime reliability contracts and failure modes

Implemented scope: bounded recorded-feed combinatorial REPLAY/PAPER/SHADOW, one existing PaperAtomic adapter and one existing paper account. Exchange-side resolution, maker async orders and multi-wallet runtime are not certified by these tests.

| Failure / trigger | Implemented response | Evidence / remaining boundary |
|---|---|---|
| Duplicate strategy intent | Native idempotency hash dedup in supervisor; no second risk/adapter submission | Executable duplicate-delivery fault. Hash namespace is one plugin/epoch; multi-plugin namespace is future work. |
| Duplicate approved order plan | Existing gateway checks native plan ID before adapter call in approved-intent and approved-decision paths | Native fake-adapter regression checks send count stays one. Unmanaged `submit(plan)` still needs consolidation. |
| Submit acknowledgment unknown | Durable SUBMIT_STARTED before OMS call; incomplete transaction freezes recovery; no automatic resend or expiry | Inject `_exit(86)` at submit boundary. Pending encumbrance remains visible; no fictitious cancellation. |
| Durable fill before crash | Journal report is fsynced before ledger application; incomplete-tail recovery restores known plan/report receipts | Inject `_exit(87)` after first durable report; one partial bundle leg restored, remaining work blocked. |
| Partial fill | Existing OMS fill tracker/partial policy retains unresolved exposure, no terminal risk disposition; supervisor freezes nonterminal result | Native partial adapter regression. Normal runtime uses atomic paper execution and does not enable asynchronous partial strategy execution. |
| Exact duplicate delta fill | Existing ledger dedup; fill tracker now deduplicates before quantity/cost accumulation | Native regression validates quantity and cash agree after duplicate and later completion. Current tuple IDs cannot distinguish every real exchange receipt; authoritative fill ID support remains a live blocker. |
| Overfill / foreign plan report | Tracker rejects fills beyond remaining lots and wrong plan | Native assertion; normalization failures must become explicit incidents before supporting live ingestion. |
| Risk reservation lifetime | Journal RISK_RESERVED before submit; complete paper terminal disposition releases existing reservation; unknown/partial stops new risk | Three modes + partial/unknown faults. No external order is freed based on elapsed TTL. Partial-tail recovery displays conservative encumbrance separately, not a falsely restored active exchange order. |
| Filled inventory/cash reused | Existing ledger supplies canonical holdings; existing inventory/exposure guards check actual sized legs; funded cash precheck | Repeat opportunity reaches limits despite released prior order reservations. No new risk or accounting framework. |
| Connection lost / new connection epoch | Clear snapshot admission; explicit reconnect needed after disconnect; require new snapshots for all configured artifact assets | Executable reconnect regression. Requiring all assets is deliberately conservative; per-subscription groups are a later optimization. |
| Corrupt schema / state failure | Record failure, clear snapshot admission; no further order until valid fresh recovery | Decode failure regression. Existing state/risk integrity gates still apply. |
| Stale / future book | Logical-clock age/quality guards reject; explicit tick records stale incident | Executable age regression. Recorded logical time is not wall-clock feed health; network daemon timer integration remains future work. |
| Monotonic clock regression | Input transaction fails and requires recovery review | Input validation; no silent reordering. Raw CRC failures fail before runtime starts. |
| Global stop / kill switch | Stop new submissions at transaction boundary; journal terminal supervisor stop; latch across recovery | Recorded stop and file-kill regression. File removal is insufficient to clear a stop. No external cancel is claimed. |
| SIGINT/SIGTERM | Finish current bounded transaction, commit, journal stop and publish state; resume remaining input with same executable | Executable signal/drain/restart test. Disk blocking has no hard real-time drain bound; no distributed cancellation claim. |
| Marks before first order plan | Bind artifact asset indices before ingestion; do not alias multiple assets to default index zero | Asymmetric YES/NO price regression validates unrealized PnL and restart. |
| Local reconciliation mismatch | Compare reported paper account to canonical ledger; journal mismatch, freeze and latch | Executable recorded-account fault. This is an internal paper comparison, not a private exchange account connection. |
| Configuration/input/artifact change | Refuse recovery when SHA-256 frozen identity differs | Separate integration tests for all three. Binary identity is frozen too; software upgrades require a reviewed migration/new epoch. |
| Conditional settlement masks without chain authority | Refuse plugin startup | Conditional-artifact regression. Original test artifact remains unchanged; new unconditional YES/NO fixture generated with existing exporter. |
| Torn or corrupted journal | Validate every line/sequence/hash; reject without truncation | Tail and corruption tests. fsync persistence is POSIX/local-filesystem dependent, not guaranteed by every remote filesystem. |
| Second runtime writer | Nonblocking exclusive advisory lock on journal before replay/append | Lock test. Assumes cooperative local processes and secure filesystem access; no multi-host lease is claimed. |
| Journal write/fsync failure | Throw and stop; do not continue after failed durable boundary | Write ordering implemented; actual ENOSPC/device-failure injection is not exercised. A post-write fsync error may leave an unknown durable tail; operator review required. |
| Telemetry failure | Stop transaction progression if snapshot publication fails; prior journal remains recovery evidence | Conservative first slice; bounded async publication/SLOs are future work. |
| LIVE config or unintegrated plugin | Refuse before creating a trading journal | Negative startup tests. Runtime constructs PaperExecutionAdapter only and contains no live transport dependency reachable from its entry. |
| Cancellation / settlement without exchange receipt | No external path enabled; report exchange reconciliation false and live realized PnL null | Native cancellation-disabled check and runtime mode tests. Existing paper cancellation is explicitly simulated. |

## Journal protocol

Records are JSONL envelopes `{seq, prev, event, sha256}`. Hash input is `previous_hash + "\n" + decimal_seq + "\n" + serialized_event`; each append writes fully and fsyncs. File creation/atomic telemetry rename sync the parent directory. Record order is:

```text
RUN_CONFIG (schema/config/input/artifact/executable/policy identities)
RUNTIME_STARTED
INPUT_BEGIN (index and frozen input record hash)
  MARKET_STATE / STRATEGY_INTENT / ORDER_DECISION / RISK_DECISION
  RISK_RESERVED
  SUBMIT_STARTED
  PLAN_PREPARED (native client/order IDs and quantities/limits)
  EXECUTION_REPORT (delta fill, status, source timestamp)
  RESERVATION_DISPOSITION / OMS_RESULT
INPUT_COMMIT (transaction event digest and account digest)
RUNTIME_STOPPED
```

The source feed is frozen and separately hashed, not copied into every journal row. Recovery therefore requires the original file. A prepared order is not an exchange ACK. No event synthesized by this runtime represents exchange settlement. Completed paper transactions reconstruct deterministically without appending duplicate receipts; an incomplete tail is preserved and blocked even if its durable subset happens to contain apparently terminal reports. Unknown economic exposure is not cleared by guessing.

Configuration and binary identities prevent replay under silently changed behavior. Hash chains detect corruption, but do not prove malicious tampering is impossible. Journals currently grow without compaction and are read into bounded-run memory at recovery; a long-running production process requires retention/checkpoint design and measured memory limits.

## Test execution

```bash
ctest --test-dir build -R RuntimeFoundation --output-on-failure
python3 tests/integration/runtime/runtime_integration.py \
  --binary build/trading_runtime --source . --evidence-dir /tmp/polytope-runtime-fault-evidence
build/runtime_lifecycle_tests
```

The Python suite launches the actual C++ executable for each scenario; it does not mirror its implementation in a Python runtime. The native regression uses a fake adapter to inject partial/duplicate reports into the existing gateway and ledger. Synthetic results establish lifecycle invariants only. Full-suite results and known unrelated failures are reported in the audit evidence directory.

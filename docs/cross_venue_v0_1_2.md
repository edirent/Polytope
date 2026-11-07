# Cross-Venue V0.1.2: As-Of state and economic discovery

The authoritative market state remains `SynchronizedBookStore`. `JointBookAsOf` is a value view of that store at a monotonic decision time, not a second book, risk, ledger or execution engine. Enable it with `asof_enabled` in the existing research config. Existing defaults preserve the V0/V0.1 legacy gates and golden outputs.

The new gate separates transport, reconstruction, authority, freshness, market status and available asks. It requires a real WS source, validated initial state, consistent version/epoch evidence, current liveness, acceptable authority age, and tradable markets. Empty or one-sided real books are reconstructible but absent asks cannot qualify an acquisition. Kalshi may omit an empty side in real fractional snapshots; this is not a broken subscription. Crossed/malformed books, sequence gaps and unresolved reconciliation failures still invalidate state.

Last-update skew remains diagnostic. Legacy reported last-update skew and legacy strategy authoritative-validation skew are separately documented. Enabled As-Of strategies remove timestamp-coincidence as a general hard gate. The current research policies are:

| Policy | Liveness | Authority | Last actual change | Legacy update skew | Strict strategy |
|---|---:|---:|---:|---|---|
| legacy comparison | 30s | 45s | 15s | <=100ms | historical comparison |
| balanced | 30s | 45s | 15s | diagnostic only | only after existing complete approval checks |
| conservative | 15s | 10s | 5s | diagnostic only | only after existing complete approval checks |
| permissive research | 30s | 90s | 60s | diagnostic only | prohibited |

An unchanged quiet book may exceed its change-age threshold only when a sufficiently recent authoritative snapshot/reconciliation confirms state **at or after** its last actual change. A live socket cannot extend authority or certify an unconfirmed quiet state. Policy sensitivity is evaluated on exactly the same raw input. Coverage is measured over intervals split at receive/processing-availability and policy expiration boundaries.

`cross_venue_economic_discovery` is a native C++20 diagnostic CLI. It uses frozen capture metadata, validates raw inventory/payload hashes and global arrival order, reconstructs through the existing `ResearchState`, and computes both complementary directions with exact micro-share VWAP and the existing market-scoped fee engine. It never manufactures an `ApprovedPair`, invokes a live trading endpoint, or assigns contractual PnL to an unapproved candidate. Approved inputs must use the existing strict replay; the diagnostic CLI refuses to silently report them as zero-approved inputs.

Money and depth use integer microcurrency/microshares and int128 products. Diagnostic quantities begin with the maximum observed minimum and common verified step when available. A missing step is explicitly an **unverified conditional probe**, not a certified order quantity. Visible depth is the best-64 observed prefix, not guaranteed or economically profitable capacity. Historical fee validity is respected. Kalshi per-fragment cent rounding is an explicit conservative non-direct-member budget; actual membership/grouping/rebates are unknown. Forward deterioration (.002/share) and conditional asset conversion (.003/share) are separate assumptions; depth walking is not charged twice.

Price results carry `UNAPPROVED_PRICE_DIAGNOSTIC`. Nominal unit parity is not a proven payout floor across different rules/assets. Gross, fee-adjusted and budget-adjusted positive states are deduplicated into episodes by candidate, direction, policy and quantity. Quantity variants overlap and must not be counted as independent trades. Latency 0/100/250/500/1000ms uses the latest state available by the future execution time and reevaluates health/depth. Missing future coverage is unknown. An empty positive-net denominator produces null survival, never a successful fill.

Maker results carry `MAKER_QUOTE_FEASIBILITY_ONLY`: conditional ceilings, tick/post-only checks, local bid/ask, opposite-depth/fee cost, measured quote-state persistence, ceiling changes and 1000ms future hedge-cost drift. No fills, queue position, rebates, execution capacity or maker PnL are inferred. Independent $100K wallets remain the capital model; diagnostic cost requirements do not allocate or recycle cash. Annual target tables are arithmetic requirements, not annualized recordings.

Nine archived candidate dossiers are refreshed against official APIs and retain full rule text, PDF byte/text hashes, exception-state counterexamples and pending human identity/decision fields. The existing Oracle implication builder is called for an explicitly unverified draft constraint; no payoff state is excluded and nothing is injected into the executable strategy. Complete human revision, payoff, collateral and quantity approval remain mandatory.

New captures write a hashed `processing_timings.jsonl` sidecar keyed by raw record sequence/hash. Replay verifies it and delays effective availability until processing completion. Original exchange and local timestamps are never rewritten. Historical captures lack this completion field; their original serialized-receive availability is retained and this precision limit is disclosed. Clock tools record OS synchronization, current offset/root-distance estimate, public HTTP RTT and local callback/processing measurements without modifying the system clock or subtracting a fixed historical offset.

Reproduce the old capture into a **new** directory:

```bash
cmake -S . -B build
cmake --build build -j4
build/cross_venue_economic_discovery \
  --run-dir runs/cross_venue/v0_1_1_authenticated_20261105_3/capture \
  --out /tmp/polytope-v012-new-analysis
python3 research/cross_venue/audit_v0_1_2.py \
  --source runs/cross_venue/v0_1_1_authenticated_20261105_3/capture \
  --out /tmp/polytope-v012-new-analysis
ctest --test-dir build --output-on-failure -j4
```

The bounded nine-pair 1200s launcher reuses local WSL environment variables and the existing signing connector:

```bash
python3 research/cross_venue/secure_kalshi_environment.py launch-economic
```

Do not pass credentials in arguments or chat. This command is read-only and finite; no daemon is installed. The existing `launch` workflow remains available unchanged. All runs, including interrupted/failed evidence, remain separately identifiable. No unrelated strategy source or pre-existing market-making failure is changed by V0.1.2.

Measured output and the final recommendation are in `runs/cross_venue/v0_1_2_20261106/report.md`, with source/config/analysis hashes in the accompanying manifests.

# Cross-Venue V0.1.3 — Generalized payoff research

V0.1.3 extends the existing native cross-venue research stack with bounded 2–4-leg payoff relationships. It is read-only. A conditional normal-state payout floor is not a contractual guarantee, a human approval, a fill, or realized PnL.

Measured evidence and the Chinese A–I report are in [the research run](../runs/cross_venue/v0_1_3_20261106/research_report.md). The exact implementation file list with purposes and SHA-256 is [implementation_changes.json](../runs/cross_venue/v0_1_3_20261106/implementation_changes.json).

## Architecture

- `oracle/payoff/GeneralizedPayoff.{h,cpp}` extends the existing Oracle StateEnumerator/PayoffMatrixBuilder. Boolean constraints use the existing compiler. At most four Boolean variables and four distinct purchased instruments are enumerated exhaustively, plus explicit exceptional settlement branches and alternative cases. An empty state space is rejected. Exact portfolio payouts round down per contract before summation.
- `engine/strategy/cross_venue/GeneralizedDiscovery.cpp` turns official metadata into canonical predicates, generates bounded relationships, and saves deterministic rule-bound proof/counterexample dossiers. Normal proposed constraints and source-supported normal constraints are separate. Missing exceptional states remain explicit uncertainty envelopes. The narrow football classifier only supports ordinary completed-match win/draw/win partitions; NegRisk membership is not proof of arbitrary convertibility.
- `GeneralizedOptimizer.cpp` reuses FractionalVwap and existing market-specific fee calculations. It enumerates a reported quantity grid, enforces known minimums/steps/ticks, independent wallet limits, total capital and intermediate cash risk. Gross, fee and budget stages keep separate sizing witnesses. Zero exposure is always available. A loose sound box relaxation bounds the bounded quantity box; continuous global optimality is not claimed.
- `GeneralizedReplay.cpp` uses existing ResearchState/SynchronizedBookStore/JointBookAsOf and original hash-verified capture records. Processing completion is the availability cutoff. Cached immutable views do not independently reconstruct books. Known transport errors disable eligibility before reconnection. Validity expiries split coverage intervals. Five latency scenarios hold original quantities fixed. Episodes are deduplicated; missing future data stays unknown.
- `reserve_generalized` uses existing venue RiskLedgers. It requires complete approved contractual evidence, known assets/quantities and an evidenced capital-release bound. Reservations hold until actual modeled settlement. Instruments cannot be consumed again. There is no order submission, automatic fill or synthetic paper profit.
- Existing pair execution/paper ledger code remains intact. Multi-leg execution output is an intermediate-inventory scenario analysis. Real multi-leg execution integration is blocked by zero approved relationships.
- Python scripts orchestrate finite collection, independently recheck all raw WS books/payoffs/fees/sizing, and generate reports. They are not an alternative strategy or book engine.

## Limits and interpretation

The search currently generates complements, structured greater/greater-or-equal threshold ladders, candidate time-window implications, exclusive/exhaustive outcome hypotheses, and two-, three- and four-leg cross-venue templates. Title/entity similarity can propose a hypothesis but never supplies a contractual proof. Exact index/time/inequality/exception compatibility remains a rule-review requirement.

Default limits are 512 candidate groups, 40 per type with venue diversity, 4 legs, 6 sizing points per leg, 10,000 evaluations, 250ms per optimization and 1,000 shares per leg. Search truncation and grid limits are reported, not described as exhaustive. New-market screening uses liquidity proxy >=1,000 and trading-close proxy between 1 hour and 14 days. Actual settlement/cash release are distinct and can be unknown.

Default capital is $200k with configurable $100k per venue. Unhedged acquisition cash plus fees is capped at $5k. Forward budget is 0.002 per purchased share; cross-venue template conversion budget adds 0.003 per purchased share. The conversion budget remains charged if sizing reduces the active template to one venue. Unknown asset identity/conversion is still a strict blocker; a budget does not prove a depeg bound.

Books use existing balanced As-Of limits: 30s connection liveness, 45s authoritative evidence, 15s book-change authority rule. Market-specific fees must be valid at the true decision/execution wall time. Kalshi fragmented notional cash ceilings are reconciled with aggregate VWAP rounding to avoid a one-microcurrency under-reservation.

Missing quantity precision may allow explicitly conditional diagnostic probes; it never permits strict orders. Missing instruments are `MISSING_MARKET_DATA_OR_RULE_REVISION`, not zero-profit observations. An uncertainty-envelope zero payout is a conservative bound, not an assertion that an exchange will actually pay zero.

## Reproduction

Use separate empty output directories. No credentials are needed for historical replay.

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4

build/cross_venue_discover --config config/cross_venue_v0_1_3.json --run-dir runs/cross_venue/v013_new_discovery
build/run_generalized_payoff_research --phase discover --config config/cross_venue_v0_1_3.json --universe runs/cross_venue/v013_new_discovery/market_metadata.json --out runs/cross_venue/v013_new_proofs

build/run_generalized_payoff_research --phase replay --config config/cross_venue_v0_1_3.json --relationships runs/cross_venue/v0_1_3_20261106/proofs/payoff_proofs.jsonl --run-dir runs/cross_venue/v0_1_2_focused_20261106_2/capture --out runs/cross_venue/v013_new_replay
python3 research/cross_venue/audit_v0_1_3.py --proofs runs/cross_venue/v0_1_3_20261106/proofs/payoff_proofs.jsonl --source runs/cross_venue/v0_1_2_focused_20261106_2/capture --analysis runs/cross_venue/v013_new_replay
```

Repeat the same replay into another empty output and give the independent audit `--repeat SECOND_DIRECTORY`. Frozen proof hash verification and every deterministic output are checked. Do not overwrite the frozen proofs used by the prepared capture plan. Fresh rules require new artifacts and human revision approval.

`CrossVenueV013_IndependentRealL2OptimizerAndDeterminism` uses the locally available real recording and frozen proof set. It returns CTest skip code 77 if those optional evidence files are absent. The other generalized tests use explicitly synthetic temporary fixtures.

## Finite secure collection

The prepared config targets 12 Kalshi and 5 Polymarket new markets, covering CPI, sports outcomes and 3/4-leg review-score thresholds. Existing capture selection may fill spare per-venue slots with real controls; the exact subscribed instruments are always in each capture's `selected_markets.json`. Public preflight logs rejected one-sided/expired markets and hashes every response. None of the selections are approved for trading.

In the WSL terminal containing the user's existing environment, use:

```bash
python3 /home/edirent/Polytope/research/cross_venue/secure_kalshi_environment.py launch-generalized
```

It uses `KALSHI_API_KEY_ID` and `KALSHI_PRIVATE_KEY_PATH` (key fallback `/home/edirent/.secrets/kalshi.pem`). Missing Key ID is entered through local hidden input. Credentials remain process-local, are not written into reports or command arguments, and are never requested in chat. This starts exactly one 1,200-second native read-only capture followed by native feed audit and generalized replay, then exits. It validates the prepared proof-file hash before collection. No indefinite collector is installed.

A 5-second public-only native decoder probe was performed during implementation: Kalshi was REST-only because this Codex process did not inherit the Key ID. It is explicitly excluded from primary authenticated economic results and does not satisfy the requested 20-minute Phase C. Refer to the run report for actual launch/completion status; do not infer success from a launcher message alone.

## Artifacts and provenance

Root CSVs include `capture_id` and `data_scope`. Join opportunities to `portfolio_costs.csv` within that capture using `source_evidence_portfolio_id` or `portfolio_id`; costs contain exact source token/market, rules hash, book/authority/liveness timestamps, raw hash/offset, all retained visible asks, market quantity/tick metadata and fee source/version/validity. Join the relationship to `payoff_proofs.jsonl` using relationship ID and artifact hash.

`strategy_comparison.csv` contains overlapping strategic classes; `relationship_economic_types_summary.csv` preserves native per-type metrics. `active_portfolio_structure.csv` distinguishes configured legs from nonzero active legs and actual venue scope. `gross_opportunities.csv` records quantities optimizing gross, while `portfolio_candidates.csv` records the best nonzero budget objective. Negative diagnostics are not executed; abstention objective is zero.

`capital_efficiency.csv` and `conditional_candidate_ranking.csv` leave actual release, capital-day profit and strict deployment capacity unknown. `capital_target_requirements.csv` is hypothetical turnover arithmetic, not a projection. Maker fields keep fill probability and realized PnL null. No observed fill receipts exist.

`audit_manifest.json` hashes root reports, original source inventories, frozen proofs, independent audits and implementation hashes. It excludes itself from recursive hashing. The existing unrelated `AvalancheSkewsQuotesAndRiskCutsBuyChannel` test failure is reported separately and remains unchanged.

Official discovery references: [Kalshi markets](https://docs.kalshi.com/api-reference/market/get-markets), [Kalshi pagination](https://docs.kalshi.com/getting_started/pagination), [Polymarket market discovery](https://docs.polymarket.com/market-data/discover-markets). Actual source response URL and pagination evidence are saved under the run's `discovery/`.

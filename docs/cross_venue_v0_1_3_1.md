# V0.1.3.1 — Real-market validation and economic decision

This milestone reuses the V0.1.3 architecture and frozen 191 proof artifacts. It does not add an order transport or automatic approval. The measured report is [economic_feasibility.md](../runs/cross_venue/v0_1_3_1_20261106/economic_feasibility.md).

The previously prepared secure launcher was already running when this task began. The same acquisition completed 1,200.133351725 seconds, recorded 27,922 raw records, and exited. Its original immutable evidence is `runs/cross_venue/v0_1_3_focused_20261106/capture/`. No duplicate capture was started. New results reference its raw inventory rather than copying the 48.6 MB recording.

All 12 intended Kalshi markets and five intended Polymarket markets were selected, with seven separately labeled fallback controls. Kalshi produced 24 book snapshots and 6,926 deltas. The 11 preselected structural groups all had priced observations. Exact outcome/token identity, subscription evidence, snapshots, acquisition-side deltas, epoch recovery, minimum/step availability, fees and counterpart As-Of durations are in `selected_market_coverage.csv` and `subscription_validation.csv`. The outbound subscription payload was not retained; attempted subscription is supported by the frozen selected set and source code, accepted data by actual snapshots. A Polymarket snapshot is evidence of market delivery, not an invented explicit ACK.

Native interval coverage uses the existing ResearchState and JointBookAsOf. It splits at every relevant authority/change/liveness expiry and records compressed intervals. A sweep over intended markets measures simultaneous healthy two-venue availability separately from first-to-last message overlap. It does not label REST snapshots as continuous L2. Quiet but valid reconstructed books need not receive repeated changes.

## Small, additive implementation

`GeneralizedQuantityVerification.cpp` calls the existing optimizer with fixed quantities; it does not reconstruct books or replace the optimizer. Verification is enabled only for explicitly listed observed portfolio IDs. The default V0.1.3 six-point optimizer and replay outputs remain intact.

Two-leg relationships whose exhaustively enumerated normal payout is exactly `min(q1,q2)` use merged depth breakpoints, minimum/step neighborhoods, fee-rounding neighbors and a cash boundary. Verification can expand beyond the baseline 1,000-share limit to the retained observed depth while preserving actual venue wallets and the configured cash-at-risk limit. Larger single-leg depth beyond the other leg's maximum cannot improve this minimum payout. Every evaluated vector and every relevant retained-depth breakpoint is reported.

A sound continuous lower bound on acquisition cash and model fees gives gross, fee and budget upper bounds. The ordinary payout objective is affine between merged depth breakpoints; endpoint maxima therefore bound the whole stated box. Unknown minimum/step restrictions are relaxed only for the bound and remain blockers for real orders. A zero upper bound matching abstention proves no positive conditional objective within that observed box; a positive bound is not a feasible trade. The proof is conditional on the stated normal-state relationship and fee model. It is not a contractual guarantee, fill receipt or platform-solvency proof.

For small multi-leg portfolios, a complete known discrete domain is enumerated only when bounded and practical; otherwise the existing grid is retained and the global gap stays unknown. Best two-, three- and four-active-leg values are recorded separately. Intermediate inventory scenarios remain research only.

Solver elapsed time is measured in `quantity_solver_timings.jsonl`. This timing file is intentionally excluded from byte determinism; all economic/coverage/proof outputs are compared byte for byte. Runtime and evaluation limits, unsupported precision, search scope and null gaps remain visible.

## Fee interpretation

The inherited Kalshi per-fragment cent-ceiling calculation is a conservative pre-rebate cash reserve. Official fee-rounding documentation describes an order-level accumulator, nonnegative net fees and precision-dependent rebates; a reserve is not an actual observed final fee. The new continuous model-fee lower bound does not count rounding refunds as extra profit. Account precision and fill receipts are unavailable, so no exact realized fee or PnL is claimed. Unverified maker/taker rewards are excluded. Market-specific frozen coefficients and their validity windows are retained.

## Rule review and approval

At most five intensive packages are selected from observed economics, executable depth, new-market coverage, ordinary rule support, close-time proxy and leg count. Structural duplicates are limited. Complete official market PDFs, public policy responses, rule hashes, ordinary state vectors, special-branch findings and failed authoritative-source requests are recorded under the run. The Kalshi regulatory landing request returned 429 and the attempted official v1.29 archive URL returned 404; neither is silently treated as a retrieved effective rulebook.

Current URL-linked contract terms are not cryptographic proof of their historical effective version. Special fair-price branches require joint payout constraints across separate contracts. Polymarket general collateral descriptions do not bind an individual condition to a specific collateral/adapter contract. Missing minimum/step fields remain unknown. No dossier closes every exceptional branch; none is approved. The original revision/hash-bound pending manifest and all V0.1.3 artifacts are preserved.

## Reproduction and audit

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

`CrossVenueV0131_IndependentRealQuantityBreakpointsAndBounds` replays a locally available real historical record, independently recomputes the new quantity/bound results and compares core outputs. Missing optional historical artifacts produce CTest skip 77. The native synthetic test additionally constructs a small profitable depth segment deliberately missed by a sparse grid; its 0.30 synthetic result never enters observed economics.

`research/cross_venue/validate_v0_1_3_1.py` assembles native baseline and targeted verification, calls the V0.1.3 independent audit and `audit_v0_1_3_1.py`, checks subscription selection and interval unions, prepares dossiers, performs conservative shared-liquidity allocation, and writes the ten mandatory conclusions. It never loads credentials. All raw inputs remain referenced by original path and hash. Native snapshots of source at capture time remain in the original recording; current implementation and result hashes are in the new audit manifest.

Recorded commands and small configs are saved in the result directory. To repeat quantity verification, use the saved `verification_config.json`, frozen V0.1.3 proof file, original completed capture, and a new empty output directory with `build/run_generalized_payoff_research --phase replay`. The root baseline and verification cost/coverage tables are checked for byte identity; only the additive quantity outputs change. Original capture and V0.1.3 result directories must not be overwritten.

Real order submission remains disabled. `verified_live_realized_pnl = null`. Conditional tested capacity and hypothetical annual-turnover arithmetic are reported separately from approved deployment capacity.

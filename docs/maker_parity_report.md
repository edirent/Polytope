# Maker parity scope and method

Actual counts/deviations are generated in runs/runtime_v0_2_1_20261106/strategy_parity.csv and final_report.md after executable validation. Do not treat this method description as a passing result.

The new MakerPlugin calls the unchanged MarketMakingEngine used by existing maker runners. The standalone native runtime_maker_parity tool instantiates that same legacy core independently and consumes frozen immutable frames with identical configuration hash, clock, book versions/depth, inventory and feedback resets. It compares full result hashes, native desired quote fields (prices, sizes, fair value, inventory targets, refresh metadata) and cancel hashes. Source recordings remain immutable.

STRATEGY_FRAME records precede portfolio admission. STRATEGY_DESIRED_RESET records reproduce legacy runner feedback after own simulated fills and quote-risk rejection. This prevents a different inventory/feedback path from being mislabeled as an algorithm deviation.

The comparison boundary is the generic maker runner core, not every specialized BTC 5m/15m product provider, external-fair feature builder or old runner's immediate-replace execution orchestration. Those broader runner comparisons remain unverified. No algorithm was edited to force parity. Differences in executable quotes can arise from explicitly versioned outward tick rounding, shared-capital rejection and conservative cancel/replace; the original desired quote remains preserved.

Reproduce with: build/runtime_maker_parity PROFILE_JSON RUN/events.jsonl OUTPUT.csv. Zero compared frames is a failure, never an invented parity pass. Reports separate synthetic frames from frozen real recording frames and retain any deviations.

## Completed validation evidence

Final RC3 comparison: 118,518 immutable native frames, zero result-hash, desired-quote or cancel-hash differences. Breakdown: 92,812 frozen historical real frames, 2,556 synthetic frames and 23,150 final balanced live frames. Broader specialized product-runner orchestration remains unverified. The standalone reader preserves full unsigned 64-bit provenance hashes. Earlier candidate comparisons are retained separately and are not added to this final count.

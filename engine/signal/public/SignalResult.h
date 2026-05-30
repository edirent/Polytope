#pragma once

#include "engine/signal/metrics/SignalMetrics.h"

#include <cstdint>

namespace trading_engine::signal {

struct SignalRunResult {
    std::uint64_t bundles_scanned = 0;

    std::uint64_t rejected_invalid_settlement = 0;
    std::uint64_t rejected_bad_market_state = 0;
    std::uint64_t rejected_missing_snapshot = 0;
    std::uint64_t rejected_insufficient_depth = 0;
    std::uint64_t rejected_low_edge = 0;
    std::uint64_t duplicate_intents = 0;
    std::uint64_t rate_limited = 0;
    std::uint64_t rejected_duplicate = 0;
    std::uint64_t rejected_rate_limited = 0;
    std::uint64_t rejected_snapshot_skew = 0;
    std::uint64_t rejected_stale_snapshot = 0;

    std::uint64_t vwap_checked = 0;
    std::uint64_t edge_computed = 0;

    std::uint64_t paper_opportunities = 0;
    std::uint64_t intents_published = 0;

    std::uint64_t output_hash = 0;
    SignalMetrics metrics;
};

}  // namespace trading_engine::signal

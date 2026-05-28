#pragma once

#include <cstdint>

namespace trading_engine::signal {

struct SignalRunResult {
    std::uint64_t bundles_scanned = 0;

    std::uint64_t rejected_invalid_settlement = 0;
    std::uint64_t rejected_bad_market_state = 0;
    std::uint64_t rejected_missing_snapshot = 0;
    std::uint64_t rejected_insufficient_depth = 0;
    std::uint64_t rejected_low_edge = 0;

    std::uint64_t paper_opportunities = 0;
    std::uint64_t intents_published = 0;

    std::uint64_t output_hash = 0;
};

}  // namespace trading_engine::signal

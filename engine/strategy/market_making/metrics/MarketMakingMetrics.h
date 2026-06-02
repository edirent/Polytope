#pragma once

#include <cstdint>

namespace trading_engine::strategy::market_making {

struct MarketMakingMetrics {
    std::uint64_t snapshots_seen = 0;
    std::uint64_t quotes_emitted = 0;
    std::uint64_t cancels_emitted = 0;
    std::uint64_t replacements = 0;
    std::uint64_t no_quote = 0;

    void reset() noexcept {
        snapshots_seen = 0;
        quotes_emitted = 0;
        cancels_emitted = 0;
        replacements = 0;
        no_quote = 0;
    }
};

}  // namespace trading_engine::strategy::market_making

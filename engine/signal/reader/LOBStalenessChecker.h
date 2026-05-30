#pragma once

#include "state/MarketStateSnapshot.h"

#include <cstdint>

namespace trading_engine::signal {

class LOBStalenessChecker {
public:
    [[nodiscard]] bool is_stale(
        const trading_engine::state::MarketStateSnapshot& snapshot,
        std::uint64_t now_ns,
        std::int64_t max_age_ns
    ) const noexcept;

    [[nodiscard]] std::uint64_t age_ns(
        const trading_engine::state::MarketStateSnapshot& snapshot,
        std::uint64_t now_ns
    ) const noexcept;
};

}  // namespace trading_engine::signal

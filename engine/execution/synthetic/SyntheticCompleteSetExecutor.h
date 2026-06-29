#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>
#include <string>

namespace trading_engine::execution::synthetic {

struct SyntheticCompleteSetInput {
    strategy::market_making::OutcomeSide rich_leg_side =
        strategy::market_making::OutcomeSide::No;
    std::int64_t rich_leg_bid_tick = 0;
    std::int64_t rich_leg_tradable_fair_tick = 0;
    std::int64_t cheap_leg_tradable_fair_tick = 0;
    std::int64_t residual_inventory_lots = 0;
    std::int64_t target_residual_inventory_lots = 0;
    std::int64_t available_cash_tick = 0;
    std::int64_t min_edge_tick = 10'000;
    std::int64_t min_exit_depth_lots = 1;
    std::int64_t exit_depth_lots = 0;
    std::int64_t tte_ns = 0;
};

struct SyntheticCompleteSetDecision {
    bool should_mint_and_sell_rich_leg = false;
    std::int64_t synthetic_cheap_leg_cost_tick = 0;
    std::int64_t expected_edge_tick = 0;
    std::string reason;
};

class SyntheticCompleteSetExecutor {
public:
    [[nodiscard]] SyntheticCompleteSetDecision evaluate_paper(
        const SyntheticCompleteSetInput& input
    ) const noexcept;
};

}  // namespace trading_engine::execution::synthetic

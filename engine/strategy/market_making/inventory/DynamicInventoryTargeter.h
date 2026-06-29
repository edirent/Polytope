#pragma once

#include "engine/strategy/market_making/fair/ExternalFairMarketSpec.h"

#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

struct InventoryTargetInput {
    ExternalFairEventType event_type = ExternalFairEventType::Unknown;
    std::int64_t canonical_yes_market_mid_tick = 0;
    std::int64_t canonical_yes_external_fair_tick = 0;
    std::int64_t canonical_yes_tradable_fair_tick = 0;
    std::int64_t spread_tick = 0;
    std::int64_t book_depth_lots = 0;
    std::int64_t tte_ns = 0;
    int confidence_bps = 0;
    double implied_vol = 0.0;
    double realized_vol = 0.0;
    std::int64_t current_canonical_yes_position_lots = 0;
    std::int64_t portfolio_touch_exposure_lots = 0;
};

struct InventoryTargetOutput {
    std::int64_t target_canonical_yes_lots = 0;
    std::int64_t min_canonical_yes_lots = 0;
    std::int64_t max_canonical_yes_lots = 0;
    std::int64_t inventory_skew_tick = 0;
    std::string reason;
};

class DynamicInventoryTargeter {
public:
    [[nodiscard]] InventoryTargetOutput compute(
        const InventoryTargetInput& input
    ) const;

private:
    static constexpr std::int64_t kTicksPerCent = 10'000;
};

}  // namespace trading_engine::strategy::market_making

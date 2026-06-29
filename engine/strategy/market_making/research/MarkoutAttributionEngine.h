#pragma once

#include <cstdint>

namespace trading_engine::strategy::market_making::research {

struct FillAttributionInput {
    std::int64_t fill_price_tick = 0;
    std::int64_t fair_at_fill_tick = 0;
    std::int64_t mid_at_fill_tick = 0;
    std::int64_t future_mid_tick = 0;
    std::int64_t future_fair_tick = 0;
    bool buy = true;
};

struct FillAttribution {
    std::int64_t fill_price_tick = 0;
    std::int64_t fair_at_fill_tick = 0;
    std::int64_t mid_at_fill_tick = 0;
    std::int64_t markout_tick = 0;
    std::int64_t fair_markout_tick = 0;
    std::int64_t spread_capture_tick = 0;
    std::int64_t adverse_selection_tick = 0;
    std::int64_t inventory_pnl_tick = 0;
    std::int64_t reward_pnl_tick = 0;
};

class MarkoutAttributionEngine {
public:
    [[nodiscard]] FillAttribution attribute(
        const FillAttributionInput& input
    ) const noexcept;
};

}  // namespace trading_engine::strategy::market_making::research

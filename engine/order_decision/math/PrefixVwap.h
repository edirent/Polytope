#pragma once

#include "engine/state/view/MarketDepthView.h"

#include <cstdint>

namespace trading_engine::order_decision {

struct PrefixVwapResult {
    bool ok = false;

    std::int64_t qty_lots = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t vwap_tick = 0;
    std::int64_t worst_price_tick = 0;

    std::uint16_t level_index = 0;
};

[[nodiscard]] PrefixVwapResult buy_vwap_from_prefix(
    const trading_engine::state::MarketDepthView& view,
    std::int64_t qty_lots
) noexcept;

}  // namespace trading_engine::order_decision

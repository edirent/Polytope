#pragma once

#include "engine/state/book/DepthPrefix.h"
#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"

#include <algorithm>
#include <cstdint>

namespace trading_engine::strategy::market_making {

struct QuoteSizeResult {
    bool ok = false;
    std::int64_t bid_qty_lots = 0;
    std::int64_t ask_qty_lots = 0;
    const char* reason = "";
};

class QuoteSizeModel {
public:
    [[nodiscard]] QuoteSizeResult compute(
        const MarketMakingConfig& config,
        const state::MarketDepthView& depth,
        std::int64_t current_position_lots
    ) const noexcept {
        QuoteSizeResult result;
        if (config.base_quote_size_lots <= 0) {
            result.reason = "base quote size <= 0";
            return result;
        }

        const auto ask_depth = state::ask_depth_from_prefix(depth.prefix);
        const auto bid_depth = state::bid_depth_from_prefix(depth.prefix);
        const auto bid_room =
            std::max<std::int64_t>(0, config.max_inventory_lots -
                                          current_position_lots);
        const auto ask_room =
            std::max<std::int64_t>(0, current_position_lots -
                                          config.min_inventory_lots);

        if (config.enable_bid_quotes && bid_room > 0 && ask_depth > 0) {
            result.bid_qty_lots =
                std::min({config.base_quote_size_lots, bid_room, ask_depth});
        }
        if (config.enable_ask_quotes && ask_room > 0 && bid_depth > 0) {
            result.ask_qty_lots =
                std::min({config.base_quote_size_lots, ask_room, bid_depth});
        }
        result.ok = result.bid_qty_lots > 0 || result.ask_qty_lots > 0;
        if (!result.ok) {
            result.reason = "no inventory room or depth";
        }
        return result;
    }
};

}  // namespace trading_engine::strategy::market_making

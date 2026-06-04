#pragma once

#include "engine/state/book/DepthPrefix.h"
#include "engine/state/view/MarketDepthView.h"
#include "engine/strategy/market_making/public/MarketMakingConfig.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

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
        const auto long_room = std::max<std::int64_t>(
            1,
            config.max_inventory_lots - config.target_position_lots
        );
        const auto short_room = std::max<std::int64_t>(
            1,
            config.target_position_lots - config.min_inventory_lots
        );
        const auto inventory_error =
            current_position_lots - config.target_position_lots;
        const auto denominator =
            inventory_error >= 0 ? long_room : short_room;
        const auto ratio = std::clamp(
            static_cast<long double>(inventory_error) /
                static_cast<long double>(denominator),
            -1.0L,
            1.0L
        );
        const auto max_multiplier = std::max<long double>(
            1.0L,
            static_cast<long double>(config.max_quote_size_multiplier_bps) /
                10'000.0L
        );
        const auto bid_multiplier =
            std::clamp(1.0L - ratio, 0.0L, max_multiplier);
        const auto ask_multiplier =
            std::clamp(1.0L + ratio, 0.0L, max_multiplier);
        const auto scaled_bid_size = static_cast<std::int64_t>(
            std::llround(
                static_cast<long double>(config.base_quote_size_lots) *
                bid_multiplier
            )
        );
        const auto scaled_ask_size = static_cast<std::int64_t>(
            std::llround(
                static_cast<long double>(config.base_quote_size_lots) *
                ask_multiplier
            )
        );

        if (config.enable_bid_quotes && bid_room > 0 && ask_depth > 0 &&
            scaled_bid_size > 0) {
            result.bid_qty_lots =
                std::min({scaled_bid_size, bid_room, ask_depth});
        }
        if (config.enable_ask_quotes && ask_room > 0 && bid_depth > 0 &&
            scaled_ask_size > 0) {
            result.ask_qty_lots =
                std::min({scaled_ask_size, ask_room, bid_depth});
        }
        result.ok = result.bid_qty_lots > 0 || result.ask_qty_lots > 0;
        if (!result.ok) {
            result.reason = "no inventory room or depth";
        }
        return result;
    }
};

}  // namespace trading_engine::strategy::market_making

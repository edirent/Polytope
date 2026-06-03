#pragma once

#include "engine/state/view/MarketDepthView.h"

#include <cstdint>

namespace trading_engine::paper {

enum class MarkQuality : std::uint8_t {
    Good,
    MissingBid,
    MissingAsk,
    MissingBook,
    Degraded,
    NoPosition
};

struct MarkPrice {
    std::uint32_t asset_index = 0;

    std::int64_t mid_mark_tick = 0;
    std::int64_t liquidation_mark_tick = 0;

    MarkQuality mid_quality = MarkQuality::MissingBook;
    MarkQuality liquidation_quality = MarkQuality::MissingBook;
};

class MarkPriceProvider {
public:
    [[nodiscard]] MarkPrice mark_from_depth(
        const trading_engine::state::MarketDepthView& view
    ) const noexcept;
};

}  // namespace trading_engine::paper

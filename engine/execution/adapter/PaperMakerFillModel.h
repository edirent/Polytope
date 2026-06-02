#pragma once

#include "engine/execution/public/ExecutionTypes.h"
#include "engine/execution/public/MakerExecutionTypes.h"
#include "engine/state/view/MarketDepthView.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::execution {

struct PaperMakerMarketEvent {
    std::uint64_t ts_ns = 0;
    std::uint32_t asset_index = 0;
    const state::MarketDepthView* depth = nullptr;

    bool has_trade = false;
    std::int64_t trade_price_tick = 0;
    std::int64_t trade_qty_lots = 0;
    OrderSide trade_aggressor_side = OrderSide::Buy;
};

struct MakerFillResult {
    bool filled = false;

    std::uint64_t quote_id = 0;
    std::uint64_t approved_quote_id = 0;
    std::uint64_t quote_group_id = 0;

    std::uint32_t asset_index = 0;
    std::string asset_id;

    QuoteSide side = QuoteSide::Bid;

    std::int64_t filled_qty_lots = 0;
    std::int64_t fill_price_tick = 0;

    std::uint64_t ts_ns = 0;

    std::string reason;
};

class PaperMakerFillModel {
public:
    [[nodiscard]] std::vector<MakerFillResult> evaluate(
        const PaperMakerQuote& quote,
        const PaperMakerMarketEvent& event,
        PaperMakerFillMode mode,
        bool allow_partial_fills = true,
        std::int64_t max_fill_qty_per_trade = 0
    ) const;
};

}  // namespace trading_engine::execution

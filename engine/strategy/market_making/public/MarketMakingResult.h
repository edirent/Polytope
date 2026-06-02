#pragma once

#include "engine/strategy/market_making/public/CancelQuoteIntent.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <array>
#include <cstdint>

namespace trading_engine::strategy::market_making {

struct MarketMakingResult {
    bool ok = true;

    std::uint64_t snapshots_seen = 0;
    std::uint64_t quotes_emitted = 0;
    std::uint64_t cancels_emitted = 0;
    std::uint64_t replacements = 0;
    std::uint64_t rejected_no_quote = 0;
    std::uint64_t active_quotes = 0;
    std::uint64_t approved_quotes = 0;
    std::uint64_t rejected_quotes = 0;
    std::uint64_t maker_fills = 0;

    std::int64_t avg_half_spread_tick = 0;
    std::int64_t avg_inventory_skew_tick = 0;
    std::uint64_t quote_uptime_ns = 0;

    std::int64_t maker_realized_pnl_tick = 0;
    std::int64_t maker_unrealized_pnl_tick = 0;
    std::int64_t spread_capture_tick = 0;
    std::int64_t adverse_selection_5s_tick = 0;

    std::uint16_t quote_count = 0;
    std::array<QuoteIntent, 2> quotes{};

    std::uint16_t cancel_count = 0;
    std::array<CancelQuoteIntent, 2> cancels{};

    std::uint64_t output_hash = 0;
};

[[nodiscard]] std::uint64_t compute_market_making_result_hash(
    const MarketMakingResult& result
) noexcept;

}  // namespace trading_engine::strategy::market_making

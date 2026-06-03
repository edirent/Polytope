#pragma once

#include "engine/paper/pnl/MarkPriceProvider.h"

#include <cstdint>

namespace trading_engine::paper {

struct MakerPnLSnapshot {
    std::uint64_t ts_ns = 0;

    std::int64_t maker_realized_pnl_tick = 0;
    std::int64_t maker_unrealized_pnl_mid_tick = 0;
    std::int64_t maker_unrealized_pnl_liquidation_tick = 0;

    std::int64_t equity_mid_tick = 0;
    std::int64_t equity_liquidation_tick = 0;

    std::int64_t cash_tick = 0;
    std::int64_t fees_paid_tick = 0;

    std::uint64_t maker_fill_count = 0;

    MarkQuality mark_quality = MarkQuality::NoPosition;
};

}  // namespace trading_engine::paper

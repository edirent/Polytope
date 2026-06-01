#pragma once

#include <cstdint>

namespace trading_engine::paper {

struct PaperPnL {
    std::int64_t realized_pnl_tick = 0;
    std::int64_t unrealized_pnl_tick = 0;
    std::int64_t mark_to_market_pnl_tick = 0;
    std::int64_t fees_tick = 0;
    std::int64_t slippage_tick = 0;

    std::uint64_t version = 0;
    std::uint64_t updated_ts_ns = 0;
};

}  // namespace trading_engine::paper

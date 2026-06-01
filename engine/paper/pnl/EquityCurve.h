#pragma once

#include <cstdint>

namespace trading_engine::paper {

struct EquityCurve {
    std::int64_t equity_mid_tick = 0;
    std::int64_t equity_liquidation_tick = 0;

    std::int64_t realized_pnl_tick = 0;
    std::int64_t unrealized_pnl_mid_tick = 0;
    std::int64_t unrealized_pnl_liquidation_tick = 0;

    std::uint64_t ts_ns = 0;
};

}  // namespace trading_engine::paper

#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::paper {

struct Position {
    std::string asset_id;
    std::uint32_t asset_index = 0;

    std::int64_t qty_lots = 0;
    std::int64_t avg_cost_tick = 0;
    std::int64_t cost_basis_tick = 0;

    std::int64_t realized_pnl_tick = 0;
    std::int64_t unrealized_pnl_mid_tick = 0;
    std::int64_t unrealized_pnl_liquidation_tick = 0;
};

}  // namespace trading_engine::paper

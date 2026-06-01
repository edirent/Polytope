#pragma once

#include <cstddef>
#include <cstdint>

namespace trading_engine::paper {

struct ExposureView {
    std::size_t asset_count = 0;
    std::int64_t total_qty_lots = 0;
    std::int64_t gross_cost_basis_tick = 0;
    std::int64_t unrealized_pnl_mid_tick = 0;
    std::int64_t unrealized_pnl_liquidation_tick = 0;
};

}  // namespace trading_engine::paper

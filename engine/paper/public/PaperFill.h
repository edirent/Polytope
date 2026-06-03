#pragma once

#include "engine/execution/public/ExecutionTypes.h"
#include "engine/execution/public/MakerExecutionTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::paper {

using FillLiquidityRole = trading_engine::execution::FillLiquidityRole;
using Side = trading_engine::execution::OrderSide;

struct PaperFill {
    std::uint64_t fill_id = 0;
    std::uint64_t report_id = 0;

    std::uint64_t plan_id = 0;
    std::uint64_t order_id = 0;

    std::uint64_t quote_id = 0;
    std::uint64_t approved_quote_id = 0;
    std::uint64_t quote_group_id = 0;

    std::uint32_t asset_index = 0;
    std::string asset_id;

    Side side = Side::Buy;

    FillLiquidityRole liquidity_role = FillLiquidityRole::Unknown;

    std::int64_t qty_lots = 0;
    std::int64_t fill_price_tick = 0;
    std::int64_t fee_tick = 0;

    std::uint64_t ts_ns = 0;

    std::uint64_t idempotency_hash = 0;
};

}  // namespace trading_engine::paper

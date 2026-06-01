#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ExecutionTypes.h"

#include <cstdint>
#include <string>

namespace trading_engine::paper {

struct FillApplication {
    trading_engine::execution::ExecutionReport report;

    std::uint64_t execution_report_id = 0;

    std::string market_id;
    std::string asset_id;
    std::uint32_t asset_index = 0;
    trading_engine::execution::OrderSide side =
        trading_engine::execution::OrderSide::Buy;

    std::int64_t fee_tick = 0;
};

[[nodiscard]] std::uint64_t derive_execution_report_id(
    const FillApplication& fill
) noexcept;

}  // namespace trading_engine::paper

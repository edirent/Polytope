#pragma once

#include "engine/order_decision/public/OrderDecisionTypes.h"

#include <array>
#include <cstdint>
#include <string>

namespace trading_engine::order_decision {

inline constexpr std::uint16_t kMaxCostCurveLevels = 64;

struct CostCurveLevel {
    std::int64_t price_tick = 0;
    std::int64_t size_lots = 0;
    std::int64_t cumulative_qty_lots = 0;
    std::int64_t cumulative_cost_tick = 0;
};

struct CostCurve {
    std::string market_id;
    std::string asset_id;
    std::uint32_t asset_index = 0;
    Side side = Side::Buy;

    std::uint16_t level_count = 0;
    std::array<CostCurveLevel, kMaxCostCurveLevels> levels{};

    std::int64_t total_qty_lots = 0;
};

struct CostForQuantityResult {
    bool ok = false;
    std::int64_t quantity_lots = 0;
    std::int64_t total_cost_tick = 0;
    std::int64_t vwap_tick = 0;
    std::int64_t worst_price_tick = 0;
};

struct CostForQuantityStats {
    std::uint32_t depth_levels_scanned = 0;
};

[[nodiscard]] CostForQuantityResult cost_for_quantity(
    const CostCurve& curve,
    std::int64_t quantity_lots,
    CostForQuantityStats* stats = nullptr
) noexcept;

}  // namespace trading_engine::order_decision

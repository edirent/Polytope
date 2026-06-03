#pragma once

#include "engine/paper/pnl/AdverseSelectionTracker.h"
#include "engine/paper/public/PaperFill.h"

#include <cstdint>
#include <optional>
#include <span>

namespace trading_engine::paper {

struct MakerFillMetricInput {
    PaperFill fill;
    std::optional<std::int64_t> mid_at_fill_tick;
};

struct MakerPerformanceMetricsInput {
    std::span<const MakerFillMetricInput> fills;
    std::span<const AdverseSelectionRecord> adverse_selection_records;

    std::int64_t maker_realized_pnl_tick = 0;
    std::int64_t maker_unrealized_pnl_mid_tick = 0;
    std::int64_t maker_unrealized_pnl_liquidation_tick = 0;

    std::uint64_t quote_count = 0;
    std::uint64_t cancel_replace_count = 0;
};

struct MakerPerformanceMetricsSnapshot {
    std::uint64_t maker_fill_count = 0;
    std::uint64_t maker_bid_fill_count = 0;
    std::uint64_t maker_ask_fill_count = 0;

    std::int64_t maker_realized_pnl_tick = 0;
    std::int64_t maker_unrealized_pnl_mid_tick = 0;
    std::int64_t maker_unrealized_pnl_liquidation_tick = 0;

    std::int64_t spread_capture_tick = 0;
    std::int64_t adverse_selection_5s_tick = 0;
    std::int64_t adverse_selection_30s_tick = 0;

    double quote_fill_rate = 0.0;
    double cancel_replace_rate = 0.0;

    std::uint64_t missing_mid_at_fill_count = 0;
};

class MakerPerformanceMetrics {
public:
    [[nodiscard]] MakerPerformanceMetricsSnapshot compute(
        const MakerPerformanceMetricsInput& input
    ) const;
};

}  // namespace trading_engine::paper

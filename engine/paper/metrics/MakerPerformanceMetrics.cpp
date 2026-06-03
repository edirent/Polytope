#include "engine/paper/metrics/MakerPerformanceMetrics.h"

namespace trading_engine::paper {

namespace {

[[nodiscard]] std::int64_t spread_capture_for_fill(
    const PaperFill& fill,
    std::int64_t mid_at_fill_tick
) noexcept {
    if (fill.side == Side::Sell) {
        return (fill.fill_price_tick - mid_at_fill_tick) * fill.qty_lots;
    }
    return (mid_at_fill_tick - fill.fill_price_tick) * fill.qty_lots;
}

[[nodiscard]] double ratio(
    std::uint64_t numerator,
    std::uint64_t denominator
) noexcept {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

}  // namespace

MakerPerformanceMetricsSnapshot MakerPerformanceMetrics::compute(
    const MakerPerformanceMetricsInput& input
) const {
    MakerPerformanceMetricsSnapshot snapshot;
    snapshot.maker_realized_pnl_tick = input.maker_realized_pnl_tick;
    snapshot.maker_unrealized_pnl_mid_tick =
        input.maker_unrealized_pnl_mid_tick;
    snapshot.maker_unrealized_pnl_liquidation_tick =
        input.maker_unrealized_pnl_liquidation_tick;

    for (const auto& item : input.fills) {
        if (item.fill.liquidity_role != FillLiquidityRole::Maker) {
            continue;
        }
        ++snapshot.maker_fill_count;
        if (item.fill.side == Side::Sell) {
            ++snapshot.maker_ask_fill_count;
        } else {
            ++snapshot.maker_bid_fill_count;
        }

        if (!item.mid_at_fill_tick) {
            ++snapshot.missing_mid_at_fill_count;
            continue;
        }
        snapshot.spread_capture_tick +=
            spread_capture_for_fill(item.fill, *item.mid_at_fill_tick);
    }

    for (const auto& record : input.adverse_selection_records) {
        if (record.status_5s == AdverseSelectionStatus::Ready) {
            snapshot.adverse_selection_5s_tick +=
                record.adverse_selection_5s_tick;
        }
        if (record.status_30s == AdverseSelectionStatus::Ready) {
            snapshot.adverse_selection_30s_tick +=
                record.adverse_selection_30s_tick;
        }
    }

    snapshot.quote_fill_rate =
        ratio(snapshot.maker_fill_count, input.quote_count);
    snapshot.cancel_replace_rate =
        ratio(input.cancel_replace_count, input.quote_count);
    return snapshot;
}

}  // namespace trading_engine::paper

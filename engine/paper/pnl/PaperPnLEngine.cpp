#include "engine/paper/pnl/PaperPnLEngine.h"

namespace trading_engine::paper {

namespace {

[[nodiscard]] const trading_engine::state::MarketDepthView* find_depth(
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    std::uint32_t asset_index
) noexcept {
    for (const auto& view : depth_views) {
        if (view.asset_index == asset_index) {
            return &view;
        }
    }
    return nullptr;
}

[[nodiscard]] MarkQuality worse(MarkQuality lhs, MarkQuality rhs) noexcept {
    return static_cast<std::uint8_t>(lhs) >= static_cast<std::uint8_t>(rhs)
        ? lhs
        : rhs;
}

}  // namespace

PaperPnLEngine::PaperPnLEngine(MarkPriceProvider mark_provider)
    : mark_provider_(mark_provider) {}

PaperPnLResult PaperPnLEngine::compute(
    const PaperPortfolio& portfolio,
    const CashLedger& cash,
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    std::uint64_t ts_ns
) const {
    PaperPnLResult result;
    result.realized_pnl_tick = cash.realized_pnl_tick;
    result.worst_mark_quality = MarkQuality::Good;
    std::int64_t mid_position_value_tick = 0;
    std::int64_t liquidation_position_value_tick = 0;

    for (const auto& [_, position] : portfolio.positions().positions()) {
        const auto* depth = find_depth(depth_views, position.asset_index);
        MarkPrice mark;
        if (depth != nullptr) {
            mark = mark_provider_.mark_from_depth(*depth);
        }

        const auto mid_value = position.qty_lots * mark.mid_mark_tick;
        const auto liquidation_value =
            position.qty_lots * mark.liquidation_mark_tick;
        mid_position_value_tick += mid_value;
        liquidation_position_value_tick += liquidation_value;

        result.unrealized_pnl_mid_tick += mid_value - position.cost_basis_tick;
        result.unrealized_pnl_liquidation_tick +=
            liquidation_value - position.cost_basis_tick;

        result.worst_mark_quality = worse(result.worst_mark_quality, mark.mid_quality);
        result.worst_mark_quality =
            worse(result.worst_mark_quality, mark.liquidation_quality);
    }

    result.equity.realized_pnl_tick = result.realized_pnl_tick;
    result.equity.unrealized_pnl_mid_tick = result.unrealized_pnl_mid_tick;
    result.equity.unrealized_pnl_liquidation_tick =
        result.unrealized_pnl_liquidation_tick;
    result.equity.equity_mid_tick =
        cash.cash_tick + mid_position_value_tick;
    result.equity.equity_liquidation_tick =
        cash.cash_tick + liquidation_position_value_tick;
    result.equity.ts_ns = ts_ns;
    return result;
}

}  // namespace trading_engine::paper

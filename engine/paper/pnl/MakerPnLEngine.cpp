#include "engine/paper/pnl/MakerPnLEngine.h"

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

[[nodiscard]] std::uint8_t mark_rank(MarkQuality quality) noexcept {
    switch (quality) {
        case MarkQuality::Good:
            return 0;
        case MarkQuality::MissingBid:
            return 1;
        case MarkQuality::MissingAsk:
            return 2;
        case MarkQuality::MissingBook:
            return 3;
        case MarkQuality::Degraded:
            return 4;
        case MarkQuality::NoPosition:
            return 0;
    }
    return 4;
}

[[nodiscard]] MarkQuality worse(MarkQuality lhs, MarkQuality rhs) noexcept {
    return mark_rank(lhs) >= mark_rank(rhs) ? lhs : rhs;
}

}  // namespace

MakerPnLEngine::MakerPnLEngine(MarkPriceProvider mark_provider)
    : mark_provider_(mark_provider) {}

MakerPnLSnapshot MakerPnLEngine::compute(
    const PaperLedger& ledger,
    std::span<const trading_engine::state::MarketDepthView> depth_views,
    std::uint64_t ts_ns
) const {
    MakerPnLSnapshot snapshot;
    snapshot.ts_ns = ts_ns;
    snapshot.maker_realized_pnl_tick =
        ledger.cash_ledger().realized_pnl_tick;
    snapshot.cash_tick = ledger.cash_ledger().cash_tick;
    snapshot.fees_paid_tick = ledger.cash_ledger().fees_paid_tick;
    snapshot.maker_fill_count = ledger.snapshot().maker_fill_count;
    snapshot.equity_mid_tick = ledger.cash_ledger().cash_tick;
    snapshot.equity_liquidation_tick = ledger.cash_ledger().cash_tick;

    bool has_open_position = false;
    MarkQuality quality = MarkQuality::Good;

    for (const auto& [_, position] : ledger.position_ledger().positions()) {
        if (position.qty_lots <= 0) {
            continue;
        }
        has_open_position = true;

        MarkPrice mark;
        if (const auto* depth = find_depth(depth_views, position.asset_index)) {
            mark = mark_provider_.mark_from_depth(*depth);
        }

        const auto mid_position_value =
            position.qty_lots * mark.mid_mark_tick;
        const auto liquidation_position_value =
            position.qty_lots * mark.liquidation_mark_tick;

        snapshot.maker_unrealized_pnl_mid_tick +=
            mid_position_value - position.cost_basis_tick;
        snapshot.maker_unrealized_pnl_liquidation_tick +=
            liquidation_position_value - position.cost_basis_tick;
        snapshot.equity_mid_tick += mid_position_value;
        snapshot.equity_liquidation_tick += liquidation_position_value;

        quality = worse(quality, mark.mid_quality);
        quality = worse(quality, mark.liquidation_quality);
    }

    snapshot.mark_quality =
        has_open_position ? quality : MarkQuality::NoPosition;
    if (!has_open_position) {
        snapshot.maker_unrealized_pnl_mid_tick = 0;
        snapshot.maker_unrealized_pnl_liquidation_tick = 0;
        snapshot.equity_mid_tick = ledger.cash_ledger().cash_tick;
        snapshot.equity_liquidation_tick = ledger.cash_ledger().cash_tick;
    }
    return snapshot;
}

}  // namespace trading_engine::paper

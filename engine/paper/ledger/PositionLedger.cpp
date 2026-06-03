#include "engine/paper/ledger/PositionLedger.h"

namespace trading_engine::paper {

PositionLedgerApplyResult PositionLedger::apply_fill(const PositionFill& fill) {
    if (fill.side == trading_engine::execution::OrderSide::Sell) {
        return {false, PositionLedgerApplyStatus::UnsupportedSell};
    }
    if (fill.asset_id.empty() || fill.qty_lots <= 0 || fill.price_tick < 0) {
        return {false, PositionLedgerApplyStatus::InvalidFill};
    }

    apply_buy(
        fill.asset_id,
        fill.asset_index,
        fill.qty_lots,
        fill.qty_lots * fill.price_tick
    );
    return {true, PositionLedgerApplyStatus::Applied};
}

void PositionLedger::apply_buy(
    const std::string& asset_id,
    std::int64_t lots,
    std::int64_t cost_tick
) {
    apply_buy(asset_id, 0, lots, cost_tick);
}

void PositionLedger::apply_buy(
    const std::string& asset_id,
    std::uint32_t asset_index,
    std::int64_t lots,
    std::int64_t cost_tick
) {
    auto& position = positions_[asset_id];
    position.asset_id = asset_id;
    position.asset_index = asset_index;
    position.qty_lots += lots;
    position.cost_basis_tick += cost_tick;
    if (position.qty_lots > 0) {
        position.avg_cost_tick = position.cost_basis_tick / position.qty_lots;
    }
}

PositionLedgerApplyResult PositionLedger::apply_sell(
    const std::string& asset_id,
    std::uint32_t asset_index,
    std::int64_t lots,
    std::int64_t price_tick
) {
    if (asset_id.empty() || lots <= 0 || price_tick < 0) {
        return {false, PositionLedgerApplyStatus::InvalidFill, 0};
    }

    auto it = positions_.find(asset_id);
    if (it == positions_.end() || it->second.qty_lots < lots) {
        return {false, PositionLedgerApplyStatus::InsufficientPosition, 0};
    }

    auto& position = it->second;
    if (position.asset_index == 0) {
        position.asset_index = asset_index;
    }

    const auto avg_cost_tick = position.avg_cost_tick;
    const auto realized_pnl_tick = (price_tick - avg_cost_tick) * lots;
    position.qty_lots -= lots;
    position.cost_basis_tick -= avg_cost_tick * lots;
    position.realized_pnl_tick += realized_pnl_tick;

    if (position.qty_lots == 0) {
        position.cost_basis_tick = 0;
        position.avg_cost_tick = 0;
    } else {
        position.avg_cost_tick = position.cost_basis_tick / position.qty_lots;
    }

    return {true, PositionLedgerApplyStatus::Applied, realized_pnl_tick};
}

void PositionLedger::mark_mid(
    const std::string& asset_id,
    std::int64_t mid_tick
) {
    auto it = positions_.find(asset_id);
    if (it == positions_.end()) {
        return;
    }
    auto& position = it->second;
    position.unrealized_pnl_mid_tick =
        (mid_tick - position.avg_cost_tick) * position.qty_lots;
}

void PositionLedger::mark_liquidation(
    const std::string& asset_id,
    std::int64_t liquidation_tick
) {
    auto it = positions_.find(asset_id);
    if (it == positions_.end()) {
        return;
    }
    auto& position = it->second;
    position.unrealized_pnl_liquidation_tick =
        (liquidation_tick - position.avg_cost_tick) * position.qty_lots;
}

const PaperPosition* PositionLedger::find(const std::string& asset_id) const {
    const auto it = positions_.find(asset_id);
    if (it == positions_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::int64_t PositionLedger::lots(const std::string& asset_id) const {
    const auto* position = find(asset_id);
    return position == nullptr ? 0 : position->qty_lots;
}

std::int64_t PositionLedger::cost_basis_tick(const std::string& asset_id) const {
    const auto* position = find(asset_id);
    return position == nullptr ? 0 : position->cost_basis_tick;
}

std::int64_t PositionLedger::avg_cost_tick(const std::string& asset_id) const {
    const auto* position = find(asset_id);
    return position == nullptr ? 0 : position->avg_cost_tick;
}

std::size_t PositionLedger::asset_count() const noexcept {
    return positions_.size();
}

ExposureView PositionLedger::exposure() const {
    ExposureView out;
    out.asset_count = positions_.size();
    for (const auto& [_, position] : positions_) {
        out.total_qty_lots += position.qty_lots;
        out.gross_cost_basis_tick += position.cost_basis_tick;
        out.unrealized_pnl_mid_tick += position.unrealized_pnl_mid_tick;
        out.unrealized_pnl_liquidation_tick +=
            position.unrealized_pnl_liquidation_tick;
    }
    return out;
}

const std::map<std::string, PaperPosition>& PositionLedger::positions() const noexcept {
    return positions_;
}

}  // namespace trading_engine::paper

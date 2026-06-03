#include "engine/paper/ledger/PaperLedger.h"

namespace trading_engine::paper {

namespace {

[[nodiscard]] bool is_fill_status(
    trading_engine::execution::ChildOrderStatus status
) noexcept {
    using trading_engine::execution::ChildOrderStatus;
    return status == ChildOrderStatus::Filled ||
           status == ChildOrderStatus::PartiallyFilled;
}

}  // namespace

PaperLedger::PaperLedger(std::int64_t starting_cash_tick) {
    cash_.starting_cash_tick = starting_cash_tick;
    cash_.cash_tick = starting_cash_tick;
}

PaperLedgerApplyResult PaperLedger::apply_fill(const FillApplication& fill) {
    const auto report_id = derive_execution_report_id(fill);
    if (report_id == 0) {
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::InvalidFill,
            .fill_id = 0,
            .reason = "missing execution report id",
            .snapshot = snapshot()
        };
    }

    if (applied_execution_report_ids_.contains(report_id)) {
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::DuplicateExecutionReport,
            .fill_id = report_id,
            .reason = "duplicate execution report",
            .snapshot = snapshot()
        };
    }

    if (!is_fill_status(fill.report.status) || fill.report.filled_lots == 0) {
        applied_execution_report_ids_.insert(report_id);
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::IgnoredNonFill,
            .fill_id = report_id,
            .reason = "execution report has no fill",
            .snapshot = snapshot()
        };
    }

    if (fill.report.filled_lots < 0 ||
        fill.report.avg_fill_price_tick < 0 ||
        fill.fee_tick < 0 ||
        fill.asset_id.empty()) {
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::InvalidFill,
            .fill_id = report_id,
            .reason = "invalid execution fill",
            .snapshot = snapshot()
        };
    }

    const auto gross_cost =
        fill.report.filled_lots * fill.report.avg_fill_price_tick;
    const auto total_cost = gross_cost + fill.fee_tick;

    if (fill.side == trading_engine::execution::OrderSide::Sell) {
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::UnsupportedSell,
            .fill_id = report_id,
            .reason = "sell fills are unsupported for execution reports",
            .snapshot = snapshot()
        };
    }

    cash_.cash_tick -= total_cost;
    positions_.apply_buy(
        fill.asset_id,
        fill.asset_index,
        fill.report.filled_lots,
        gross_cost
    );

    cash_.fees_paid_tick += fill.fee_tick;
    applied_execution_report_ids_.insert(report_id);

    return {
        .applied = true,
        .status = PaperLedgerApplyStatus::Applied,
        .fill_id = report_id,
        .reason = {},
        .snapshot = snapshot()
    };
}

PaperLedgerApplyResult PaperLedger::apply_fill(const PaperFill& fill) {
    if (fill.fill_id == 0 ||
        fill.qty_lots <= 0 ||
        fill.fill_price_tick < 0 ||
        fill.fee_tick < 0 ||
        fill.asset_id.empty() ||
        fill.liquidity_role != FillLiquidityRole::Maker) {
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::RejectedInvalidFill,
            .fill_id = fill.fill_id,
            .reason = "invalid maker fill",
            .snapshot = snapshot()
        };
    }

    if (applied_fill_ids_.contains(fill.fill_id) ||
        (fill.report_id != 0 &&
         applied_maker_report_ids_.contains(fill.report_id))) {
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::DuplicateIgnored,
            .fill_id = fill.fill_id,
            .reason = "duplicate maker fill",
            .snapshot = snapshot()
        };
    }

    const auto gross_notional =
        fill.qty_lots * fill.fill_price_tick;

    if (fill.side == Side::Buy) {
        cash_.cash_tick -= gross_notional + fill.fee_tick;
        positions_.apply_buy(
            fill.asset_id,
            fill.asset_index,
            fill.qty_lots,
            gross_notional
        );
        cash_.fees_paid_tick += fill.fee_tick;
    } else if (fill.side == Side::Sell) {
        const auto position_result = positions_.apply_sell(
            fill.asset_id,
            fill.asset_index,
            fill.qty_lots,
            fill.fill_price_tick
        );
        if (!position_result.applied) {
            const auto status =
                position_result.status ==
                        PositionLedgerApplyStatus::InsufficientPosition
                    ? PaperLedgerApplyStatus::RejectedInsufficientPosition
                    : PaperLedgerApplyStatus::RejectedInvalidFill;
            return {
                .applied = false,
                .status = status,
                .fill_id = fill.fill_id,
                .reason = "maker sell exceeds current position",
                .snapshot = snapshot()
            };
        }

        cash_.cash_tick += gross_notional - fill.fee_tick;
        cash_.realized_pnl_tick +=
            position_result.realized_pnl_tick - fill.fee_tick;
        cash_.fees_paid_tick += fill.fee_tick;
    } else {
        return {
            .applied = false,
            .status = PaperLedgerApplyStatus::RejectedUnsupportedSide,
            .fill_id = fill.fill_id,
            .reason = "unsupported maker fill side",
            .snapshot = snapshot()
        };
    }

    applied_fill_ids_.insert(fill.fill_id);
    if (fill.report_id != 0) {
        applied_maker_report_ids_.insert(fill.report_id);
    }
    ++maker_fill_count_;

    return {
        .applied = true,
        .status = PaperLedgerApplyStatus::Applied,
        .fill_id = fill.fill_id,
        .reason = {},
        .snapshot = snapshot()
    };
}

PaperLedgerSnapshot PaperLedger::snapshot() const {
    PaperLedgerSnapshot out;
    out.cash = cash_;
    out.applied_execution_report_count = applied_execution_report_ids_.size();
    out.applied_fill_count = applied_fill_ids_.size();
    out.maker_fill_count = maker_fill_count_;
    out.position_count = positions_.asset_count();
    return out;
}

const CashLedger& PaperLedger::cash_ledger() const noexcept {
    return cash_;
}

const PositionLedger& PaperLedger::position_ledger() const noexcept {
    return positions_;
}

PaperAccount PaperLedger::account_snapshot() const {
    PaperAccount account;
    account.starting_cash_tick = cash_.starting_cash_tick;
    account.cash_balance_tick = cash_.cash_tick;
    account.reserved_cash_tick = cash_.reserved_cash_tick;
    account.realized_pnl_tick = cash_.realized_pnl_tick;
    return account;
}

PaperPnL PaperLedger::pnl_snapshot() const {
    PaperPnL pnl;
    pnl.realized_pnl_tick = cash_.realized_pnl_tick;
    pnl.fees_tick = cash_.fees_paid_tick;
    return pnl;
}

}  // namespace trading_engine::paper

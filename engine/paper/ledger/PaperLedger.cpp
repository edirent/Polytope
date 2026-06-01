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
            false,
            PaperLedgerApplyStatus::InvalidFill,
            snapshot()
        };
    }

    if (applied_execution_report_ids_.contains(report_id)) {
        return {
            false,
            PaperLedgerApplyStatus::DuplicateExecutionReport,
            snapshot()
        };
    }

    if (!is_fill_status(fill.report.status) || fill.report.filled_lots == 0) {
        applied_execution_report_ids_.insert(report_id);
        return {
            false,
            PaperLedgerApplyStatus::IgnoredNonFill,
            snapshot()
        };
    }

    if (fill.report.filled_lots < 0 ||
        fill.report.avg_fill_price_tick < 0 ||
        fill.fee_tick < 0 ||
        fill.asset_id.empty()) {
        return {
            false,
            PaperLedgerApplyStatus::InvalidFill,
            snapshot()
        };
    }

    const auto gross_cost =
        fill.report.filled_lots * fill.report.avg_fill_price_tick;
    const auto total_cost = gross_cost + fill.fee_tick;

    if (fill.side == trading_engine::execution::OrderSide::Sell) {
        return {
            false,
            PaperLedgerApplyStatus::UnsupportedSell,
            snapshot()
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
        true,
        PaperLedgerApplyStatus::Applied,
        snapshot()
    };
}

PaperLedgerSnapshot PaperLedger::snapshot() const {
    PaperLedgerSnapshot out;
    out.cash = cash_;
    out.applied_execution_report_count = applied_execution_report_ids_.size();
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

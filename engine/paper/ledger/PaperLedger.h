#pragma once

#include "engine/paper/ledger/CashLedger.h"
#include "engine/paper/ledger/FillApplication.h"
#include "engine/paper/ledger/PositionLedger.h"
#include "engine/paper/public/PaperAccount.h"
#include "engine/paper/public/PaperFill.h"
#include "engine/paper/public/PaperPnL.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace trading_engine::paper {

enum class PaperLedgerApplyStatus : std::uint8_t {
    Applied,
    DuplicateExecutionReport,
    IgnoredNonFill,
    UnsupportedSell,
    InvalidFill,
    DuplicateIgnored,
    RejectedInvalidFill,
    RejectedUnsupportedSide,
    RejectedInsufficientPosition
};

struct PaperLedgerSnapshot {
    CashLedger cash;
    std::size_t applied_execution_report_count = 0;
    std::size_t applied_fill_count = 0;
    std::uint64_t maker_fill_count = 0;
    std::size_t position_count = 0;
};

struct PaperLedgerApplyResult {
    bool applied = false;
    PaperLedgerApplyStatus status = PaperLedgerApplyStatus::InvalidFill;
    std::uint64_t fill_id = 0;
    std::string reason;
    PaperLedgerSnapshot snapshot;
};

class PaperLedger {
public:
    explicit PaperLedger(std::int64_t starting_cash_tick);

    [[nodiscard]] PaperLedgerApplyResult apply_fill(const FillApplication& fill);
    [[nodiscard]] PaperLedgerApplyResult apply_fill(const PaperFill& fill);

    [[nodiscard]] PaperLedgerSnapshot snapshot() const;
    [[nodiscard]] const CashLedger& cash_ledger() const noexcept;
    [[nodiscard]] const PositionLedger& position_ledger() const noexcept;

    [[nodiscard]] PaperAccount account_snapshot() const;
    [[nodiscard]] PaperPnL pnl_snapshot() const;

private:
    CashLedger cash_;
    PositionLedger positions_;
    std::unordered_set<std::uint64_t> applied_execution_report_ids_;
    std::unordered_set<std::uint64_t> applied_fill_ids_;
    std::unordered_set<std::uint64_t> applied_maker_report_ids_;
    std::uint64_t maker_fill_count_ = 0;
};

}  // namespace trading_engine::paper

#pragma once
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/state/cross_venue/Types.h"
#include <optional>

namespace trading_engine::cross_venue {
struct PairPnLAudit {
    std::array<std::int64_t, 2> quantities{};
    std::array<Money, 2> spend{}, closing_cash{};
    Money fees = 0, floor_pnl = 0, payout_if_yes = 0, payout_if_no = 0;
    Money suspense = 0;
    std::size_t late_receipts = 0;
    std::optional<Money> illustrative_settled_pnl;
    // Intentionally NEVER populated by offline simulation.
    std::optional<Money> verified_live_realized_pnl;
    std::vector<FillReceipt> receipts;
};

class TwoAccountPaperLedger {
public:
    TwoAccountPaperLedger(ApprovedPair pair, std::array<Money, 2> cash,
                          std::uint64_t cutoff_ns, std::uint64_t max_receipt_lag_ns = 1'000'000'000);
    bool apply(const FillReceipt& receipt);
    PairPnLAudit audit(std::optional<bool> illustrative_yes = std::nullopt,
        std::optional<std::array<Money, 2>> reported_closing_cash = std::nullopt) const;
    const paper::PaperLedger& account(std::size_t i) const { return accounts_.at(i); }
private:
    ApprovedPair pair_;
    std::array<paper::PaperLedger, 2> accounts_;
    std::uint64_t cutoff_, max_receipt_lag_;
    std::map<std::string, FillReceipt> receipts_;
    std::map<std::uint64_t, std::string> numeric_ids_;
};
}  // namespace trading_engine::cross_venue

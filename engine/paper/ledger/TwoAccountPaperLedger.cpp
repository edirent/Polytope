#include "engine/paper/ledger/TwoAccountPaperLedger.h"
#include <algorithm>

namespace trading_engine::cross_venue {
TwoAccountPaperLedger::TwoAccountPaperLedger(ApprovedPair p, std::array<Money, 2> cash, std::uint64_t cutoff, std::uint64_t lag)
    : pair_(std::move(p)), accounts_{paper::PaperLedger(cash[0]), paper::PaperLedger(cash[1])}, cutoff_(cutoff), max_receipt_lag_(lag) {
    validate_pair(pair_); require(cash[0] >= 0 && cash[1] >= 0, "invalid simulation accounts");
}
bool TwoAccountPaperLedger::apply(const FillReceipt& f) {
    validate_receipt(pair_, f);
    require(f.received_time_ns <= cutoff_, "receipt after audit cutoff");
    const auto id = f.identity();
    if (const auto it = receipts_.find(id); it != receipts_.end()) { require(it->second == f, "conflicting duplicate fill"); return false; }
    const auto numeric = compact_hash(id);
    require(numeric != 0 && (!numeric_ids_.contains(numeric) || numeric_ids_.at(numeric) == id), "numeric receipt ID collision");
    auto& account = accounts_[f.canonical_yes ? 0 : 1];
    const auto spend = add(multiply(f.price, f.qty), f.fee_paid);
    require(account.cash_ledger().cash_tick >= spend, "receipt exceeds venue-local simulation cash");
    require(add(account.position_ledger().lots(f.key.identity()), f.qty) <= max_quantity, "inventory quantity overflow");
    paper::PaperFill application;
    application.fill_id = application.report_id = numeric;
    application.asset_id = f.key.identity(); application.qty_lots = f.qty;
    application.fill_price_tick = f.price; application.fee_tick = f.fee_paid;
    application.side = execution::OrderSide::Buy; application.liquidity_role = execution::FillLiquidityRole::Taker;
    application.ts_ns = f.received_time_ns;
    require(account.apply_fill(application).applied, "existing PaperLedger rejected receipt");
    numeric_ids_.emplace(numeric, id); receipts_.emplace(id, f);
    return true;
}
PairPnLAudit TwoAccountPaperLedger::audit(std::optional<bool> settled, std::optional<std::array<Money, 2>> closing) const {
    PairPnLAudit a;
    const std::array<VenueInstrumentKey, 2> keys{pair_.leg_yes, pair_.leg_no};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& cash = accounts_[i].cash_ledger();
        a.quantities[i] = accounts_[i].position_ledger().lots(keys[i].identity());
        a.spend[i] = add(cash.starting_cash_tick, -cash.cash_tick);
        a.closing_cash[i] = cash.cash_tick;
        a.fees = add(a.fees, cash.fees_paid_tick);
        if (closing) {
            const auto difference = add((*closing)[i], -cash.cash_tick);
            // Sum absolute differences: opposite account discrepancies cannot cancel.
            a.suspense = add(a.suspense, difference >= 0 ? difference : multiply(difference, -1));
        }
    }
    a.payout_if_yes = multiply(a.quantities[0], unit); a.payout_if_no = multiply(a.quantities[1], unit);
    const auto cost = add(a.spend[0], a.spend[1]);
    a.floor_pnl = add(std::min(a.payout_if_yes, a.payout_if_no), -cost);
    if (settled) a.illustrative_settled_pnl = add(*settled ? a.payout_if_yes : a.payout_if_no, -cost);
    for (const auto& [id, f] : receipts_) {
        a.receipts.push_back(f);
        if (f.received_time_ns - f.exchange_time_ns > max_receipt_lag_) ++a.late_receipts;
    }
    return a;
}
}  // namespace trading_engine::cross_venue

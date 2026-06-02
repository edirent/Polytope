#include "engine/strategy/market_making/refresh/QuoteRefreshPolicy.h"

#include "engine/strategy/market_making/refresh/QuoteTTL.h"

#include <cstdlib>

namespace trading_engine::strategy::market_making {

QuoteRefreshDecision QuoteRefreshPolicy::evaluate(
    const ActiveQuoteState* active,
    const QuoteIntent* candidate,
    const state::MarketDepthView& depth,
    const MarketMakingConfig& config,
    std::int64_t current_position_lots,
    std::uint64_t now_ns
) const noexcept {
    QuoteRefreshDecision decision;

    const bool bad_book = !depth.usable_for_depth || depth.recovering ||
                          depth.crossed || depth.closed || depth.resolved;
    if (bad_book) {
        decision.should_cancel = active != nullptr;
        decision.reason =
            (depth.closed || depth.resolved) ? CancelReason::MarketHalted
                                             : CancelReason::BookStale;
        return decision;
    }

    if (!active) {
        decision.should_replace = candidate != nullptr;
        return decision;
    }
    if (quote_expired(now_ns, active->expires_at_ns)) {
        decision.should_cancel = true;
        decision.should_replace = candidate != nullptr;
        decision.reason = CancelReason::QuoteExpired;
        return decision;
    }
    if (!candidate) {
        decision.should_cancel = true;
        decision.reason = CancelReason::RiskDegraded;
        return decision;
    }

    const auto fair_move =
        std::llabs(candidate->fair_value_tick - active->fair_value_tick);
    if (fair_move >= config.requote_threshold_tick) {
        decision.should_cancel = true;
        decision.should_replace = true;
        decision.reason = CancelReason::FairValueMoved;
        return decision;
    }

    const auto inventory_move =
        std::llabs(current_position_lots - active->current_position_lots);
    if (inventory_move >= config.inventory_requote_threshold_lots) {
        decision.should_cancel = true;
        decision.should_replace = true;
        decision.reason = CancelReason::InventoryChanged;
        return decision;
    }

    if (candidate->idempotency_hash != active->idempotency_hash) {
        decision.should_cancel = true;
        decision.should_replace = true;
        decision.reason = CancelReason::ReplacedByNewQuote;
        return decision;
    }

    return decision;
}

}  // namespace trading_engine::strategy::market_making

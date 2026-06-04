#include "engine/strategy/market_making/refresh/QuoteRefreshPolicy.h"

#include "engine/strategy/market_making/refresh/QuoteTTL.h"

#include <algorithm>
#include <cstdlib>

namespace trading_engine::strategy::market_making {

namespace {

[[nodiscard]] bool leg_shape_matches(
    const QuoteLeg& active,
    const QuoteLeg& candidate,
    std::int64_t min_price_change_tick
) noexcept {
    const auto min_change = std::max<std::int64_t>(0, min_price_change_tick);
    return active.quantity_lots == candidate.quantity_lots &&
           std::llabs(active.price_tick - candidate.price_tick) <= min_change;
}

[[nodiscard]] bool quote_shape_matches(
    const ActiveQuoteState& active,
    const QuoteIntent& candidate,
    std::int64_t min_price_change_tick
) noexcept {
    if (active.has_bid != candidate.has_bid ||
        active.has_ask != candidate.has_ask) {
        return false;
    }
    if (active.has_bid &&
        !leg_shape_matches(
            active.bid,
            candidate.bid,
            min_price_change_tick
        )) {
        return false;
    }
    if (active.has_ask &&
        !leg_shape_matches(
            active.ask,
            candidate.ask,
            min_price_change_tick
        )) {
        return false;
    }
    return true;
}

[[nodiscard]] bool inside_requote_cooldown(
    const ActiveQuoteState& active,
    const MarketMakingConfig& config,
    std::uint64_t now_ns
) noexcept {
    return config.min_requote_interval_ns > 0 &&
           now_ns >= active.created_ts_ns &&
           now_ns - active.created_ts_ns < config.min_requote_interval_ns;
}

}  // namespace

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

    if (quote_shape_matches(
            *active,
            *candidate,
            config.min_quote_price_change_tick
        )) {
        return decision;
    }

    if (inside_requote_cooldown(*active, config, now_ns)) {
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

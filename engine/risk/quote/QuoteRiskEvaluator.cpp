#include "engine/risk/quote/QuoteRiskEvaluator.h"

#include <cstdlib>
#include <limits>

namespace trading_engine::risk {

namespace mm = strategy::market_making;

namespace {

[[nodiscard]] std::uint64_t mix(
    std::uint64_t hash,
    std::uint64_t value
) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::int64_t saturating_mul(
    std::int64_t lhs,
    std::int64_t rhs
) noexcept {
    const auto value = static_cast<__int128>(lhs) * static_cast<__int128>(rhs);
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] QuoteRiskDecision base_decision(
    const QuoteRiskInput& input
) {
    QuoteRiskDecision decision;
    if (input.quote) {
        decision.quote_intent_id = input.quote->quote_intent_id;
        decision.quote_group_id = input.quote->quote_group_id;
        decision.snapshot_version_hash = input.quote->snapshot_version_hash;
        decision.idempotency_hash = input.quote->idempotency_hash;
    }
    if (input.policy) {
        const RiskPolicySnapshot wrapper{
            .quote_risk = *input.policy
        };
        decision.policy_hash = compute_policy_hash(wrapper);
    }
    decision.decision_ts_ns = input.now_ns;
    return decision;
}

[[nodiscard]] QuoteRiskResult reject(
    const QuoteRiskInput& input,
    QuoteRiskDecisionType type,
    const char* reason,
    std::int64_t bid_notional,
    std::int64_t ask_notional
) {
    QuoteRiskResult result;
    result.decision = base_decision(input);
    result.decision.decision = type;
    result.decision.reason = reason;
    result.decision.bid_notional_tick = bid_notional;
    result.decision.ask_notional_tick = ask_notional;
    result.decision.total_notional_tick = bid_notional + ask_notional;
    result.decision.decision_id =
        compute_quote_risk_decision_hash(result.decision);
    return result;
}

[[nodiscard]] bool active_side_valid(const mm::QuoteLeg& leg) noexcept {
    return leg.price_tick > 0 && leg.quantity_lots > 0;
}

[[nodiscard]] std::int64_t notional(const mm::QuoteLeg& leg) noexcept {
    if (!active_side_valid(leg)) {
        return 0;
    }
    return saturating_mul(leg.price_tick, leg.quantity_lots);
}

}  // namespace

std::uint64_t compute_quote_risk_decision_hash(
    const QuoteRiskDecision& decision
) noexcept {
    auto hash = 14695981039346656037ULL;
    hash = mix(hash, decision.quote_intent_id);
    hash = mix(hash, decision.quote_group_id);
    hash = mix(hash, static_cast<std::uint8_t>(decision.decision));
    hash = mix(hash, decision.policy_hash);
    hash = mix(hash, decision.snapshot_version_hash);
    hash = mix(hash, decision.idempotency_hash);
    hash = mix(hash, decision.decision_ts_ns);
    hash = mix(hash, static_cast<std::uint64_t>(decision.bid_notional_tick));
    hash = mix(hash, static_cast<std::uint64_t>(decision.ask_notional_tick));
    hash = mix(hash, static_cast<std::uint64_t>(decision.total_notional_tick));
    return hash;
}

std::uint64_t compute_approved_quote_hash(const ApprovedQuote& quote) noexcept {
    auto hash = 14695981039346656037ULL;
    hash = mix(hash, quote.quote_intent_id);
    hash = mix(hash, quote.quote_group_id);
    hash = mix(hash, quote.has_bid ? 1ULL : 0ULL);
    hash = mix(hash, quote.has_ask ? 1ULL : 0ULL);
    if (quote.has_bid) {
        hash = mix(hash, mm::compute_quote_leg_hash(quote.bid));
    }
    if (quote.has_ask) {
        hash = mix(hash, mm::compute_quote_leg_hash(quote.ask));
    }
    hash = mix(hash, quote.approved_ts_ns);
    hash = mix(hash, quote.expires_at_ns);
    hash = mix(hash, quote.idempotency_hash);
    hash = mix(hash, quote.policy_hash);
    hash = mix(hash, quote.snapshot_version_hash);
    return hash;
}

QuoteRiskResult QuoteRiskEvaluator::evaluate(const QuoteRiskInput& input) const {
    if (!input.quote || !input.policy || input.quote->quote_intent_id == 0 ||
        (input.quote->type != mm::QuoteIntentType::PlaceQuote &&
         input.quote->type != mm::QuoteIntentType::ReplaceQuote)) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectInvalidQuote,
            "invalid quote input",
            0,
            0
        );
    }

    const auto* quote = input.quote;
    const auto* policy = input.policy;
    const auto bid_notional = quote->has_bid ? notional(quote->bid) : 0;
    const auto ask_notional = quote->has_ask ? notional(quote->ask) : 0;

    if (policy->quote_kill_switch_enabled) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectKillSwitch,
            "quote kill switch enabled",
            bid_notional,
            ask_notional
        );
    }

    if (quote->expires_at_ns <= input.now_ns) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectExpiredQuote,
            "quote expired",
            bid_notional,
            ask_notional
        );
    }

    if (!input.depth || !input.depth->usable_for_depth) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectBookNotUsable,
            "book not usable",
            bid_notional,
            ask_notional
        );
    }
    if (input.depth->crossed) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectCrossedBook,
            "crossed book",
            bid_notional,
            ask_notional
        );
    }
    if (input.depth->recovering || input.depth->closed || input.depth->resolved) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectBookNotUsable,
            "bad book state",
            bid_notional,
            ask_notional
        );
    }
    if (policy->max_book_age_ns > 0 &&
        input.now_ns > input.depth->last_ws_recv_ns &&
        input.now_ns - input.depth->last_ws_recv_ns > policy->max_book_age_ns) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectStaleBook,
            "stale book",
            bid_notional,
            ask_notional
        );
    }

    if (!quote->has_bid && !quote->has_ask) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectInvalidQuote,
            "quote has no active sides",
            bid_notional,
            ask_notional
        );
    }
    if ((quote->has_bid && !active_side_valid(quote->bid)) ||
        (quote->has_ask && !active_side_valid(quote->ask))) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectInvalidQuote,
            "invalid side price or quantity",
            bid_notional,
            ask_notional
        );
    }
    if (quote->has_bid && quote->has_ask &&
        quote->bid.price_tick >= quote->ask.price_tick) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectCrossedBook,
            "quote crosses itself",
            bid_notional,
            ask_notional
        );
    }

    if ((quote->has_bid && !policy->allow_bid_quotes) ||
        (quote->has_ask && !policy->allow_ask_quotes)) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectUnsupportedSide,
            "quote side disabled by policy",
            bid_notional,
            ask_notional
        );
    }

    const auto max_bid_qty = quote->has_bid ? quote->bid.quantity_lots : 0;
    const auto max_ask_qty = quote->has_ask ? quote->ask.quantity_lots : 0;
    if (policy->max_quote_qty_lots > 0 &&
        (max_bid_qty > policy->max_quote_qty_lots ||
         max_ask_qty > policy->max_quote_qty_lots)) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectExposureLimit,
            "quote quantity exceeds limit",
            bid_notional,
            ask_notional
        );
    }

    if (policy->max_asset_inventory_lots > 0) {
        const auto post_bid_position =
            input.current_position_lots + max_bid_qty;
        const auto post_ask_position =
            input.current_position_lots - max_ask_qty;
        const auto worst_abs = std::max(
            std::llabs(post_bid_position),
            std::llabs(post_ask_position)
        );
        if (worst_abs > policy->max_asset_inventory_lots) {
            return reject(
                input,
                QuoteRiskDecisionType::RejectInventoryLimit,
                "inventory limit exceeded",
                bid_notional,
                ask_notional
            );
        }
    }

    const auto total_notional = bid_notional + ask_notional;
    if (policy->max_quote_notional_tick > 0 &&
        total_notional > policy->max_quote_notional_tick) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectExposureLimit,
            "quote notional exceeds limit",
            bid_notional,
            ask_notional
        );
    }

    if (input.active_quotes_for_asset >= policy->max_active_quotes_per_asset) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectDuplicateQuote,
            "too many active quotes for asset",
            bid_notional,
            ask_notional
        );
    }

    if (quote->type == mm::QuoteIntentType::ReplaceQuote &&
        input.last_replace_ts_ns > 0 && input.now_ns > input.last_replace_ts_ns &&
        input.now_ns - input.last_replace_ts_ns <
            policy->min_replace_interval_ns) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectQuoteTooFrequent,
            "replace interval too short",
            bid_notional,
            ask_notional
        );
    }

    if ((quote->has_bid &&
         quote->fair_value_tick - quote->bid.price_tick <
             policy->min_edge_to_fair_tick) ||
        (quote->has_ask &&
         quote->ask.price_tick - quote->fair_value_tick <
             policy->min_edge_to_fair_tick)) {
        return reject(
            input,
            QuoteRiskDecisionType::RejectLowEdgeToFair,
            "edge to fair below threshold",
            bid_notional,
            ask_notional
        );
    }

    QuoteRiskResult result;
    result.decision = base_decision(input);
    result.decision.decision = QuoteRiskDecisionType::Approve;
    result.decision.reason = "approved";
    result.decision.bid_notional_tick = bid_notional;
    result.decision.ask_notional_tick = ask_notional;
    result.decision.total_notional_tick = total_notional;
    result.decision.decision_id =
        compute_quote_risk_decision_hash(result.decision);

    ApprovedQuote approved;
    approved.quote_intent_id = quote->quote_intent_id;
    approved.quote_group_id = quote->quote_group_id;
    approved.has_bid = quote->has_bid;
    approved.has_ask = quote->has_ask;
    approved.bid = quote->bid;
    approved.ask = quote->ask;
    approved.approved_ts_ns = input.now_ns;
    approved.expires_at_ns = quote->expires_at_ns;
    approved.idempotency_hash = quote->idempotency_hash;
    approved.policy_hash = result.decision.policy_hash;
    approved.snapshot_version_hash = quote->snapshot_version_hash;
    approved.approved_quote_id = compute_approved_quote_hash(approved);
    result.approved_quote = approved;
    return result;
}

}  // namespace trading_engine::risk
